// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP glue for the LocalTalk nodes; see localtalk.h. One task owns each side's UART, TashTalk
// decoder and node; s_lock covers them all against the web UI's reads and status updates from
// other tasks. W-1186: a side per socket, as ethertalk.c keeps one per link.
#include "localtalk.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/uart_select.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "lt_node.h"
#include "lt_tashtalk.h"

static const char *TAG = "lt";

#define LT_BAUD   1000000
#define LT_ALIVE_MS 100     // the sync's 11 ms, with room
#define FR_LINES  10
#define LINE_LEN  100
#define JSON_MACS 8         // Macs in /api/lt: eight, named, keep it inside its buffer
// W-1186: the UART driver's rings, internal RAM, a pair per side. A frame is at most 605
// bytes (twice that if every byte were TashTalk's escape) and arrives at the cable's 230.4
// kbit/s, 29 bytes a millisecond, into a task that reads within a tick: 4 KB holds the 140 ms
// the task can be away, the longest being the other side's write below. A PAP burst out is 8
// frames, 4.9 KB, which TashTalk's CTS lets go at the same rate: 2 KB of ring means its write
// waits about 100 ms on the cable (it waited 30 ms on 4 KB). Both sides now take what the one
// took (12 KB), and rev A half of it.
#define LT_RX_RING 4096
#define LT_TX_RING 2048
#define RX_ROUNDS  16       // 4 KB a side each pass of the task, so neither side starves

// The node, the TashTalk decoder, the log and the send buffer of a side, ~10 KB, in PSRAM
// rather than .bss, where they were internal RAM that Wi-Fi and lwIP buffer in
// (docs/traps.md, W-1094). Tasks alone touch them: the UART driver copies what
// uart_write_bytes is handed, and reads into the task's own buffer.
typedef struct {
    lt_node_t node;
    tt_rx_t   rx;
    char      fr[FR_LINES][LINE_LEN];
    uint8_t   cmd[1 + LLAP_MAX + 2];        // tt_out's, on the LocalTalk task
} lt_mem_t;

typedef struct lt_side {
    int         sock;
    int         uart, tx_pin, rx_pin, cts_pin;
    int         started;            // its node is set up (the first localtalk_start)
    volatile int open;              // its UART is installed and reaches TashTalk: not parked
    volatile int quiet;             // W-1079: switched off on the settings page
    // W-1179: parked (its socket does the printer job) or listening, what the switch last
    // asked for, for when the side may send again.
    int         want_on;
    // W-1179: listening. The node sends nothing (TashTalk answers for no node, nothing goes
    // out) until main lets it, once LocalTalk has been heard on its socket: frames sent into a
    // printer print as junk, selected or not. Frames are still counted (localtalk_frames).
    volatile int hold;
    int         serve;              // the role its node has now
    lt_cfg_t    nc;                 // the node's set-up, for coming back on
    lt_ops_t    ops;
    char        obj[NBP_NAME_MAX + 1], type[NBP_NAME_MAX + 1];
    unsigned    fr_head, fr_n;
    lt_mem_t   *mem;
    unsigned    fcs_bad, errors, tx;
    volatile uint32_t rx_good;      // W-1179: frames with a good FCS, from any node
    void      (*tap)(void *ctx, const uint8_t *f, size_t n);
    void       *tap_ctx;
    pap_client_t *client;           // W-1082: the PAP client riding the node
    lt_offers_t *offers;            // W-1088: its cable's offers, in PSRAM, for each new node
    at_owner_t  owner;              // its link, and how its node is told the printer gave up
    // W-1049: the node's counters as last seen, to tell the panel what changed.
    // The LocalTalk task alone touches these.
    void      (*on_event)(int sock, lt_event_t ev);
    lt_phase_t  seen_phase;
    uint32_t    seen_replies;
    uint8_t     seen_heard[sizeof ((lt_node_t *)0)->heard];
} lt_side_t;

static lt_side_t s_side[LT_SIDES];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
// One change of state at a time (quiet, parked, started, listening): the page's switch, the
// port task and the cable test each make them. Recursive: park and the roles go through the
// others.
static SemaphoreHandle_t s_state;
static StaticSemaphore_t s_state_buf;
static portMUX_TYPE      s_state_mux = portMUX_INITIALIZER_UNLOCKED;
// W-1082: answer as the ImageWriter, every side alike; where LaserWriters' NBP answers go.
static int        s_serve = 1;
static void     (*s_found)(int link, const nbp_tuple_t *t, const lt_route_t *r);
static void     (*s_name_check)(int link, const nbp_tuple_t *t);

static void state_take(void)
{
    if (!s_state) {
        taskENTER_CRITICAL(&s_state_mux);
        if (!s_state) s_state = xSemaphoreCreateRecursiveMutexStatic(&s_state_buf);
        taskEXIT_CRITICAL(&s_state_mux);
    }
    xSemaphoreTakeRecursive(s_state, portMAX_DELAY);
}

static void state_give(void) { xSemaphoreGiveRecursive(s_state); }

static lt_side_t *side(int sock)
{
    return sock >= 0 && sock < LT_SIDES ? &s_side[sock] : NULL;
}

// The side runs: its node is up on its TashTalk.
static int up(const lt_side_t *sd) { return sd && sd->started && sd->open; }

// Where a side's node ID is kept between starts: J4's under the key every board has had, so
// a board keeps its ID across this update.
static const char *hint_key(const lt_side_t *sd) { return sd->sock == 1 ? "node" : "node_p1"; }

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static void push(char ring[][LINE_LEN], unsigned lines, unsigned *head, unsigned *n,
                 const char *line)
{
    strncpy(ring[*head], line, LINE_LEN - 1);
    ring[*head][LINE_LEN - 1] = '\0';
    *head = (*head + 1) % lines;
    if (*n < lines) (*n)++;
}

// "rx 31B 83>200 01 | ff 53 01 ..." for the frames ring.
static void note_frame(lt_side_t *sd, const char *dir, const uint8_t *f, size_t n)
{
    char line[LINE_LEN];
    int k = snprintf(line, sizeof line, "%s %uB %u>%u %02x |", dir, (unsigned)n, f[1], f[0], f[2]);
    for (size_t i = 3; i < n && k < (int)sizeof line - 4; i++)
        k += snprintf(line + k, sizeof line - k, " %02x", f[i]);
    push(sd->mem->fr, FR_LINES, &sd->fr_head, &sd->fr_n, line);
}

// ------------------------------------------------------------- node ops
// All called on the LocalTalk task with s_lock held; ctx is the side.

static void tt_out(lt_side_t *sd, const uint8_t *f, size_t n)
{
    if (!sd->open) return;                       // parked: its UART is the printer line's
    uint8_t *cmd = sd->mem->cmd;
    size_t cl = tt_send_cmd(cmd, sizeof sd->mem->cmd, f, n);
    if (!cl) return;
    uart_write_bytes(sd->uart, cmd, cl);         // TashTalk's CTS paces the hardware FIFO
    sd->tx++;
    if (f[2] != LLAP_ENQ) note_frame(sd, "tx", f, n);
}

// The node's frames: none while listening, whatever asks (a lookup, the PAP client, an echo).
static void op_send(void *ctx, const uint8_t *f, size_t n)
{
    lt_side_t *sd = ctx;
    if (!sd->hold) tt_out(sd, f, n);
}

static void op_claim(void *ctx, uint8_t node)
{
    lt_side_t *sd = ctx;
    uint8_t cmd[33];
    if (sd->open) uart_write_bytes(sd->uart, cmd, tt_nodes_cmd(cmd, node));
    if (!node) return;
    nvs_handle_t h;
    if (nvs_open("lt", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, hint_key(sd), node);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t hint_read(const lt_side_t *sd, uint8_t hint)
{
    nvs_handle_t h;
    if (nvs_open("lt", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, hint_key(sd), &hint);
        nvs_close(h);
    }
    return hint;
}

static void op_log(void *ctx, const char *line)
{
    const lt_side_t *sd = ctx;
#if CONFIG_AIRWRITER_BOARD_2026_11
    ESP_LOGI(TAG, "port %d: %s", sd->sock + 1, line);
#else
    (void)sd;
    ESP_LOGI(TAG, "%s", line);
#endif
}

// The printer gave up mid-job (on the print task): tell the Mac.
static void cable_abort(void *ctx)
{
    lt_side_t *sd = ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_pap_abort(&sd->mem->node, now_ms());
    xSemaphoreGive(s_lock);
}

// A line for the side's log, from any task (at_owner_t's note has no ctx: one a side).
static void note_to(lt_side_t *sd, const char *line)
{
    if (!sd->started) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    op_log(sd, line);
    xSemaphoreGive(s_lock);
}
static void note_0(const char *line) { note_to(&s_side[0], line); }
static void note_1(const char *line) { note_to(&s_side[1], line); }

static void op_found(void *ctx, const nbp_tuple_t *t, const lt_route_t *r)
{
    if (s_found) s_found((int)((lt_side_t *)ctx)->owner.link, t, r);
}

static void op_name_check(void *ctx, const nbp_tuple_t *t)
{
    if (s_name_check) s_name_check((int)((lt_side_t *)ctx)->owner.link, t);
}

// The node's own connection prints on the ImageWriter; one on another
// registration is the relay's (W-1088).
static const at_relay_t *relay_conn(lt_side_t *sd)
{
    return lt_node_pap_reg(&sd->mem->node) ? at_relay() : NULL;
}

static int op_job_begin(void *ctx) { return at_job_begin(&((lt_side_t *)ctx)->owner); }
static void op_job_data(void *ctx, const uint8_t *d, size_t n)
{
    lt_side_t *sd = ctx;
    const at_relay_t *r = relay_conn(sd);
    if (r) r->data(d, n);
    else at_job_data(&sd->owner, d, n);
}
static size_t op_job_room(void *ctx)
{
    lt_side_t *sd = ctx;
    const at_relay_t *r = relay_conn(sd);
    return r ? r->room() : at_job_room(&sd->owner);
}
static void op_job_end(void *ctx, int normal)
{
    lt_side_t *sd = ctx;
    const at_relay_t *r = relay_conn(sd);
    if (r) r->end(normal);
    else at_job_end(&sd->owner, normal);
}
static int op_pap_open(void *ctx, int reg, uint64_t now)
{
    lt_side_t *sd = ctx;
    return at_relay() ? at_relay()->open((int)sd->owner.link, &sd->mem->node, reg, now) : 1;
}
static size_t op_reg_status(void *ctx, int reg, uint8_t *pstr, size_t cap)
{
    lt_side_t *sd = ctx;
    return at_relay() ? at_relay()->status((int)sd->owner.link, &sd->mem->node, reg, pstr, cap) : 0;
}
static size_t op_reg_output(void *ctx, int reg, uint8_t *buf, size_t max, int *eof, int again)
{
    (void)ctx;
    (void)reg;
    if (!at_relay()) {
        *eof = 1;
        return 0;
    }
    return at_relay()->output(buf, max, eof, again);
}

static void on_frame(void *ctx, tt_event_t ev, const uint8_t *f, size_t n)
{
    lt_side_t *sd = ctx;
    if (ev != TT_FRAME) {
        sd->errors++;
        return;
    }
    if (!llap_fcs_ok(f, n)) {
        sd->fcs_bad++;
        return;
    }
    n -= 2;
    sd->rx_good++;
    if (sd->tap) sd->tap(sd->tap_ctx, f, n);
    if (sd->quiet) return;
    // Frames to us, not the Chooser's broadcasts or other nodes' traffic.
    lt_node_t *node = &sd->mem->node;
    if (node->phase == LT_ONLINE && f[0] == node->node) note_frame(sd, "rx", f, n);
    lt_node_rx(node, f, n, now_ms());
}

// ------------------------------------------------------------- task

// From a side's UART interrupt: bytes are in its ring.
static void rx_wake(uart_port_t uart, uart_select_notif_t what, BaseType_t *woken)
{
    (void)uart;
    if (what == UART_SELECT_READ_NOTIF && s_task) vTaskNotifyGiveFromISR(s_task, woken);
}

// W-1186: one task for every side. It waits a tick at most for a byte on either UART (each
// driver's interrupt wakes it, rx_wake), then reads and runs each side under the lock, never
// blocking in a driver: a parked side's driver is deleted under the same lock.
static void lt_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, 1);
        for (int i = 0; i < LT_SIDES; i++) {
            lt_side_t *sd = &s_side[i];
            if (!up(sd)) continue;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!sd->open) {                     // parked meanwhile
                xSemaphoreGive(s_lock);
                continue;
            }
            lt_node_t *node = &sd->mem->node;
            for (int k = 0; k < RX_ROUNDS; k++) {
                int n = uart_read_bytes(sd->uart, buf, sizeof buf, 0);
                if (n <= 0) break;
                tt_rx_feed(&sd->mem->rx, buf, (size_t)n, on_frame, sd);
                if (n < (int)sizeof buf) break;
                if (k == RX_ROUNDS - 1) xTaskNotifyGive(s_task);    // more is in: no wait
            }
            if (!sd->quiet) lt_node_tick(node, now_ms());
            int online = node->phase == LT_ONLINE && sd->seen_phase != LT_ONLINE;
            int lookup = node->st.replies != sd->seen_replies;
            int fresh = 0;
            for (size_t k = 0; k < sizeof sd->seen_heard; k++) {
                if (node->heard[k] & ~node->printers[k] & ~sd->seen_heard[k]) fresh = 1;
                sd->seen_heard[k] = node->heard[k];
            }
            sd->seen_phase = node->phase;
            sd->seen_replies = node->st.replies;
            xSemaphoreGive(s_lock);
            // Outside the lock: the panel's side takes its own.
            if (sd->on_event) {
                if (online) sd->on_event(sd->sock, LT_EV_ONLINE);
                if (lookup) sd->on_event(sd->sock, LT_EV_LOOKUP);
                if (fresh)  sd->on_event(sd->sock, LT_EV_NEW_NODE);
            }
        }
    }
}

static void tt_close(int uart, int tx_pin, int rx_pin, int cts_pin)
{
    uart_driver_delete(uart);
    gpio_reset_pin(tx_pin);
    gpio_reset_pin(rx_pin);
    if (cts_pin >= 0) gpio_reset_pin(cts_pin);
}

// Opens the UART to TashTalk and sends the start-up sync. As tashrouter starts
// it: TashTalk may be halfway through a command from before this boot (an
// OTA reboot mid-frame), and 1024 no-ops return it to waiting for a command
// byte; then no optional features (the FCS is ours to compute and check).
// W-1081: TashTalk takes the sync in about 11 ms. With no chip, or one not
// running TashTalk, CTS stays up and none of it goes; a node started then
// would claim an ID and look online with no chip. So ESP_ERR_NOT_FOUND gives
// the UART and the PIC's pins back, free for ICSP.
static esp_err_t tt_open(const localtalk_cfg_t *cfg)
{
    uart_config_t uc = {
        .baud_rate  = LT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = cfg->cts_pin >= 0 ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(cfg->uart_num, LT_RX_RING, LT_TX_RING, 0, NULL, 0);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uc));
    // The socket's own UART was the printer line's a moment ago: TashTalk's is never inverted.
    if (cfg->lent) uart_set_line_inverse(cfg->uart_num, UART_SIGNAL_INV_DISABLE);
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE,
                                 cfg->cts_pin >= 0 ? cfg->cts_pin : UART_PIN_NO_CHANGE));
    uart_set_select_notif_callback(cfg->uart_num, rx_wake);

    static const uint8_t nops[64];
    for (int i = 0; i < TT_SYNC_LEN / (int)sizeof nops; i++)
        uart_write_bytes(cfg->uart_num, nops, sizeof nops);
    static const uint8_t features[] = { TT_CMD_FEATURES, 0x00 };
    uart_write_bytes(cfg->uart_num, features, sizeof features);

    if (uart_wait_tx_done(cfg->uart_num, pdMS_TO_TICKS(LT_ALIVE_MS)) != ESP_OK) {
        tt_close(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->cts_pin);
        ESP_LOGW(TAG, "TashTalk on IO%d took none of the sync", cfg->tx_pin);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t localtalk_probe(int sock, const localtalk_cfg_t *cfg)
{
    lt_side_t *sd = side(sock);
    if (!sd) return ESP_ERR_INVALID_ARG;
    state_take();
    esp_err_t err = ESP_OK;
    if (!up(sd)) {
        err = tt_open(cfg);
        if (err == ESP_OK) tt_close(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->cts_pin);
    }
    state_give();
    return err;
}

static void offers_put(lt_side_t *sd)
{
    if (sd->offers)
        lt_node_offer(&sd->mem->node, sd->offers->own, sd->offers->rel, sd->offers->n_rel,
                      sd->offers->marker);
}

static void set_quiet(lt_side_t *sd, int quiet);

static esp_err_t start_now(lt_side_t *sd, const localtalk_cfg_t *cfg)
{
    if (up(sd)) return ESP_OK;
    if (!sd->started && !sd->mem) {
        sd->mem = heap_caps_calloc(1, sizeof *sd->mem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!sd->mem) sd->mem = calloc(1, sizeof *sd->mem);    // a board with no PSRAM
        if (!sd->mem) return ESP_ERR_NO_MEM;
    }
    esp_err_t err = tt_open(cfg);
    if (err != ESP_OK) return err;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    sd->uart = cfg->uart_num;
    sd->cts_pin = cfg->cts_pin;
    sd->tx_pin = cfg->tx_pin;
    sd->rx_pin = cfg->rx_pin;

    if (sd->started) {
        // Back from parked: the node as it was left, quiet, on its socket's TashTalk again.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        tt_rx_init(&sd->mem->rx);
        sd->open = 1;
        op_log(sd, "LocalTalk on its socket again");
        xSemaphoreGive(s_lock);
        if (sd->want_on) set_quiet(sd, 0);       // listening: kept for when it is let send
        return ESP_OK;
    }

    sd->sock = (int)(sd - s_side);
    strncpy(sd->obj, cfg->object, sizeof sd->obj - 1);
    strncpy(sd->type, cfg->type, sizeof sd->type - 1);
    uint8_t hint = hint_read(sd, 0);
    sd->on_event = cfg->on_event;
    sd->owner = (at_owner_t){ at_link_of_sock(sd->sock), cable_abort, sd->sock ? note_1 : note_0, sd };
    err = at_printer_init(&cfg->printer);
    if (err != ESP_OK) {
        tt_close(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, cfg->cts_pin);
        return err;
    }

    tt_rx_init(&sd->mem->rx);
    // The Macs' names and the node's registrations in PSRAM: internal RAM is short.
    sd->nc = (lt_cfg_t){ .object = cfg->object, .type = cfg->type, .status = cfg->status,
                         .hint = hint, .seed = esp_random() | 1,
                         .names = heap_caps_calloc(LT_SEEN_SLOTS, sizeof(lt_name_t), MALLOC_CAP_SPIRAM),
                         .regs = heap_caps_calloc(LT_REG_MAX, sizeof(lt_reg_t), MALLOC_CAP_SPIRAM) };
    sd->ops = (lt_ops_t){ .ctx = sd, .send = op_send, .claim = op_claim, .log = op_log,
                          .job_begin = op_job_begin, .job_data = op_job_data,
                          .job_room = op_job_room, .job_end = op_job_end, .found = op_found,
                          .name_check = op_name_check,
                          .pap_open = op_pap_open, .reg_status = op_reg_status,
                          .reg_output = op_reg_output };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sd->open = 1;                                  // lt_node_init clears TashTalk's node bitmap
    lt_node_init(&sd->mem->node, &sd->nc, &sd->ops, now_ms());
    sd->serve = s_serve;
    lt_node_set_serve(&sd->mem->node, s_serve);
    offers_put(sd);
    lt_node_attach(&sd->mem->node, sd->client);
    if (sd->hold) {                                // W-1179: listening from the start
        sd->quiet = 1;
        sd->want_on = 1;
    }
    sd->started = 1;
    xSemaphoreGive(s_lock);

    if (!s_task && xTaskCreate(lt_task, "localtalk", 6144, NULL, 4, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    xTaskNotifyGive(s_task);
    ESP_LOGI(TAG, "LocalTalk node starting as a %s (UART%d, tx IO%d rx IO%d cts IO%d, hint %u)%s",
             cfg->type, sd->uart, cfg->tx_pin, cfg->rx_pin, cfg->cts_pin, hint,
             sd->hold ? ", listening" : "");
    return ESP_OK;
}

esp_err_t localtalk_start(int sock, const localtalk_cfg_t *cfg)
{
    lt_side_t *sd = side(sock);
    if (!sd) return ESP_ERR_INVALID_ARG;
    state_take();
    esp_err_t err = start_now(sd, cfg);
    state_give();
    return err;
}

int localtalk_running(int sock) { return up(side(sock)); }

int localtalk_any(void)
{
    for (int i = 0; i < LT_SIDES; i++) if (up(&s_side[i])) return 1;
    return 0;
}

uint32_t localtalk_frames(int sock)
{
    const lt_side_t *sd = side(sock);
    return sd ? sd->rx_good : 0;
}

int localtalk_serving(void)
{
    for (int i = 0; i < LT_SIDES; i++)
        if (up(&s_side[i]) && !s_side[i].quiet && s_side[i].serve) return 1;
    return 0;
}

// The node quiet (TashTalk answers for no node, a job cut short) or on (a node acquired anew, as
// at start), with line logged if given. Under s_state, the side running.
static void quiet_now(lt_side_t *sd, int quiet, const char *line)
{
    if (quiet == sd->quiet) return;
    lt_node_t *node = &sd->mem->node;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t now = now_ms();
    if (quiet) {
        if (node->pap.open) lt_node_pap_abort(node, now);
        if (sd->client) papc_link_down(sd->client, now);
        op_claim(sd, 0);                         // TashTalk answers for no node now
        sd->quiet = 1;
    } else {
        sd->nc.hint = hint_read(sd, node->node);
        sd->nc.status = node->status;
        sd->nc.seed = esp_random() | 1;
        lt_node_init(node, &sd->nc, &sd->ops, now);
        sd->serve = s_serve;
        lt_node_set_serve(node, s_serve);        // a new node: its roles again
        offers_put(sd);
        lt_node_attach(node, sd->client);
        sd->quiet = 0;
    }
    if (line) op_log(sd, line);
    xSemaphoreGive(s_lock);
}

// The switch, for one side: kept while it is parked or listening.
static void set_quiet(lt_side_t *sd, int quiet)
{
    quiet = quiet ? 1 : 0;
    if (!sd->started) return;
    state_take();
    if (!sd->open || sd->hold) sd->want_on = !quiet;
    else quiet_now(sd, quiet, quiet ? "the AppleTalk port is off" : "the AppleTalk port is on");
    state_give();
}

void localtalk_set_hold(int sock, int hold)
{
    lt_side_t *sd = side(sock);
    if (!sd) return;
    hold = hold ? 1 : 0;
    state_take();
    if (hold != sd->hold && !sd->started) {
        sd->hold = hold;                         // for a node started later
    } else if (hold != sd->hold && hold) {
        int on = !sd->quiet;
        sd->hold = 1;                            // first: a job cut short sends nothing either
        if (sd->open) {                          // parked, want_on is already kept
            quiet_now(sd, 1, "listening: sending nothing until LocalTalk is heard");
            sd->want_on = on;
        }
    } else if (hold != sd->hold) {
        sd->hold = 0;
        if (sd->open && sd->want_on) quiet_now(sd, 0, "LocalTalk heard: the node comes on");
    }
    state_give();
}

int localtalk_holding(int sock)
{
    const lt_side_t *sd = side(sock);
    return sd ? sd->hold : 0;
}

void localtalk_park(int sock)
{
    lt_side_t *sd = side(sock);
    if (!sd) return;
    state_take();
    if (up(sd)) {
        if (!sd->hold) {                         // listening, want_on is already kept
            int on = !sd->quiet;
            quiet_now(sd, 1, NULL);
            sd->want_on = on;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uart_wait_tx_done(sd->uart, pdMS_TO_TICKS(20));
        sd->open = 0;
        sd->tap = NULL;
        tt_close(sd->uart, sd->tx_pin, sd->rx_pin, sd->cts_pin);
        op_log(sd, "LocalTalk off its socket");
        xSemaphoreGive(s_lock);
    }
    state_give();
}

void localtalk_set_offers(int sock, const lt_offers_t *o)
{
    lt_side_t *sd = side(sock);
    if (!sd) return;
    if (!sd->offers) sd->offers = heap_caps_calloc(1, sizeof *sd->offers, MALLOC_CAP_SPIRAM);
    if (!sd->offers) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *sd->offers = *o;
    if (sd->started) offers_put(sd);
    if (s_lock) xSemaphoreGive(s_lock);
}

int localtalk_offer_id(int sock, int reg)
{
    const lt_side_t *sd = side(sock);
    if (!sd || !sd->offers || reg < 1 || reg > sd->offers->n_rel ||
        reg > (int)sizeof sd->offers->ids)
        return 0;
    return sd->offers->ids[reg - 1];
}

void localtalk_set_roles(int serve, int laser)
{
    serve = serve ? 1 : 0;
    state_take();
    int any = 0;
    for (int i = 0; i < LT_SIDES; i++) {
        lt_side_t *sd = &s_side[i];
        if (!sd->started) continue;
        any = 1;
        if (!serve && !laser) {
            set_quiet(sd, 1);
            continue;
        }
        if (sd->quiet) set_quiet(sd, 0);         // as it was; its role changes below
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sd->serve != serve) {
            lt_node_t *node = &sd->mem->node;
            // No ImageWriter here any more: a Mac printing to it is told.
            if (!serve && node->pap.open) lt_node_pap_abort(node, now_ms());
            sd->serve = serve;
            lt_node_set_serve(node, serve);
            op_log(sd, serve ? "answering as the ImageWriter" : "running for a LaserWriter only");
        }
        xSemaphoreGive(s_lock);
    }
    if (any && (serve || laser)) s_serve = serve;    // for a node acquired anew, or a side started later
    state_give();
}

void localtalk_lookup(const char *type)
{
    for (int i = 0; i < LT_SIDES; i++) {
        lt_side_t *sd = &s_side[i];
        if (!up(sd) || sd->quiet) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (sd->open) lt_node_lookup(&sd->mem->node, type, now_ms());
        xSemaphoreGive(s_lock);
    }
}

void localtalk_set_found(void (*found)(int link, const nbp_tuple_t *t, const lt_route_t *r))
{
    s_found = found;
}

void localtalk_set_name_check(void (*check)(int link, const nbp_tuple_t *t))
{
    s_name_check = check;
}

void localtalk_attach(int sock, pap_client_t *c)
{
    lt_side_t *sd = side(sock);
    if (!sd) return;
    if (!sd->started) {
        sd->client = c;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sd->client = c;
    lt_node_attach(&sd->mem->node, c);
    xSemaphoreGive(s_lock);
}

int localtalk_locked(int sock, void (*fn)(void *arg), void *arg)
{
    lt_side_t *sd = side(sock);
    if (!up(sd)) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fn(arg);
    xSemaphoreGive(s_lock);
    return 0;
}

void localtalk_set_status(int sock, uint16_t status)
{
    lt_side_t *sd = side(sock);
    if (!sd || !sd->started) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_set_status(&sd->mem->node, status);
    xSemaphoreGive(s_lock);
}

void localtalk_echo(int sock, uint8_t node)
{
    lt_side_t *sd = side(sock);
    if (!up(sd)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_echo(&sd->mem->node, node, now_ms());
    xSemaphoreGive(s_lock);
}

const uint8_t *localtalk_job(size_t *len)
{
    return at_job_bytes(len);
}

void localtalk_set_tap(int sock, void (*tap)(void *ctx, const uint8_t *f, size_t n), void *ctx)
{
    lt_side_t *sd = side(sock);
    if (!up(sd)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sd->tap = tap;
    sd->tap_ctx = ctx;
    xSemaphoreGive(s_lock);
}

// The cable test's frames go out listening or not: its cable joins AirWriter's own two ports.
void localtalk_send_raw(int sock, const uint8_t *f, size_t n)
{
    lt_side_t *sd = side(sock);
    if (!up(sd)) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tt_out(sd, f, n);
    xSemaphoreGive(s_lock);
}

// Called with the job lock held (the cable test), as parking a side is: its UART stays.
int localtalk_tt_alive(int sock, int wait_ms)
{
    lt_side_t *sd = side(sock);
    if (!up(sd)) return 0;
    TickType_t t = pdMS_TO_TICKS(wait_ms);
    return uart_wait_tx_done(sd->uart, t ? t : 1) == ESP_OK;
}

static void json_str(char **p, char *end, const char *v)
{
    if (*p < end) *(*p)++ = '"';
    for (; *v && *p < end - 2; v++) {
        if (*v == '"' || *v == '\\') *(*p)++ = '\\';
        *(*p)++ = *v;
    }
    if (*p < end) *(*p)++ = '"';
}

static void json_ring(char **p, char *end, char ring[][LINE_LEN], unsigned lines,
                      unsigned head, unsigned n)
{
    for (unsigned i = 0; i < n && *p < end; i++) {
        unsigned k = (head + lines - n + i) % lines;
        if (i && *p < end) *(*p)++ = ',';
        json_str(p, end, ring[k]);
    }
}

// The Macs heard in the last 10 minutes (W-1079), with the Macintosh name of each that gave
// one: "macs":[...] at p. Under s_lock.
static char *json_macs(char *p, char *end, const lt_node_t *n)
{
    lt_seen_t seen[JSON_MACS];
    size_t ns = lt_node_seen(n, now_ms(), LT_SEEN_MS, seen, JSON_MACS);
    if (p < end) p += snprintf(p, end - p, "\"macs\":[");
    for (size_t i = 0; i < ns && p < end; i++) {
        p += snprintf(p, end - p, "%s{\"node\":%u,\"ago\":%lu", i ? "," : "",
                      seen[i].node, (unsigned long)seen[i].ago_s);
        if (seen[i].name && p < end) {
            char u[3 * NBP_NAME_MAX + 1];
            nbp_utf8(u, sizeof u, seen[i].name);
            p += snprintf(p, end - p, ",\"name\":");
            if (p < end) json_str(&p, end, u);
        }
        if (p < end) *p++ = '}';
    }
    if (p < end) *p++ = ']';
    return p;
}

size_t localtalk_json_port(int sock, char *buf, size_t cap)
{
    lt_side_t *sd = side(sock);
    if (cap < 96) return 0;
    if (!up(sd) || sd->hold || sd->quiet) {
        int listening = up(sd) && sd->hold;
        return (size_t)snprintf(buf, cap, "\"phase\":\"%s\",\"node\":0,\"on\":%s,\"laser\":%s,\"macs\":[]",
                                listening ? "listening" : "off",
                                listening && sd->want_on && s_serve ? "true" : "false",
                                listening && sd->want_on && !s_serve ? "true" : "false");
    }
    char *p = buf, *end = buf + cap - 8;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const lt_node_t *n = &sd->mem->node;
    p += snprintf(p, end - p, "\"phase\":\"%s\",\"node\":%u,\"on\":%s,\"laser\":%s,",
                  n->phase == LT_ONLINE ? "online" : "acquiring", n->node,
                  sd->serve ? "true" : "false", sd->serve ? "false" : "true");
    p = json_macs(p, end, n);
    xSemaphoreGive(s_lock);
    if (p >= end) {                              // cut short: the row without its Macs
        return (size_t)snprintf(buf, cap, "\"phase\":\"online\",\"node\":0,\"on\":%s,\"laser\":%s,\"macs\":[]",
                                sd->serve ? "true" : "false", sd->serve ? "false" : "true");
    }
    *p = '\0';
    return (size_t)(p - buf);
}

void localtalk_json(int sock, char *buf, size_t cap)
{
    lt_side_t *sd = side(sock);
    int started = sd && sd->started;
    // W-1179: parked (its socket does the printer job), the card's "on" is its setting, kept
    // for when the socket takes LocalTalk again; nothing is heard meanwhile.
    if (started && !sd->open) {
        snprintf(buf, cap, "{\"on\":%s,\"parked\":true,\"macs\":[]}",
                 sd->want_on && s_serve ? "true" : "false");
        return;
    }
    // W-1179: listening, nothing heard on its socket yet; "on" as parked.
    if (started && sd->hold) {
        snprintf(buf, cap, "{\"on\":%s,\"phase\":\"listening\",\"laser\":%s,\"macs\":[]}",
                 sd->want_on && s_serve ? "true" : "false",
                 sd->want_on && !s_serve ? "true" : "false");
        return;
    }
    // W-1082: the AppleTalk Port card's "on" is the ImageWriter answering on
    // the cable, not a node running for a LaserWriter alone (review C1).
    if (!started || sd->quiet || !sd->serve) {
        snprintf(buf, cap, "{\"on\":false,\"quiet\":%s,\"laser\":%s}",
                 started && (sd->quiet || !sd->serve) ? "true" : "false",
                 started && !sd->quiet && !sd->serve ? "true" : "false");
        return;
    }
    char *p = buf, *end = buf + cap - 8;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t tx_free = 0;
    if (sd->open) uart_get_tx_buffer_free_size(sd->uart, &tx_free);
    int cts = sd->cts_pin >= 0 ? gpio_get_level(sd->cts_pin) : -1;
    const lt_node_t *n = &sd->mem->node;
    p += snprintf(p, end - p,
        "{\"on\":true,\"phase\":\"%s\",\"node\":%u,\"net\":%u,\"status\":\"0x%04x\","
        "\"object\":",
        n->phase == LT_ONLINE ? "online" : "acquiring", n->node, n->net,
        n->status);
    json_str(&p, end, sd->obj);
    p += snprintf(p, end - p, ",\"type\":");
    json_str(&p, end, sd->type);
    p += snprintf(p, end - p, ",\"cts\":%d,\"tx_free\":%u,\"heard\":[", cts, (unsigned)tx_free);
    for (int id = 1, first = 1; id < 255 && p < end; id++) {
        if (!(n->heard[id / 8] & (1u << (id % 8)))) continue;
        p += snprintf(p, end - p, "%s%d", first ? "" : ",", id);
        first = 0;
    }
    if (p < end) *p++ = ']';
    if (p < end) *p++ = ',';
    p = json_macs(p, end, n);
    if (p < end) p += snprintf(p, end - p,
        ",\"frames\":%lu,\"fcs_bad\":%u,\"errors\":%u,\"tx\":%u,\"for_us\":%lu,"
        "\"lookups\":%lu,\"replies\":%lu,\"statuses\":%lu,\"opens\":%lu,\"busy\":%lu,"
        "\"dup\":%lu,\"echoes\":%lu,\"mac_reads\":%lu,\"mac_answers\":%lu,\"pap\":{\"open\":%s,\"conn\":%u,\"bytes\":%lu},",
        (unsigned long)n->st.frames, sd->fcs_bad, sd->errors, sd->tx, (unsigned long)n->st.for_us,
        (unsigned long)n->st.lookups, (unsigned long)n->st.replies,
        (unsigned long)n->st.statuses, (unsigned long)n->st.opens, (unsigned long)n->st.busy,
        (unsigned long)n->st.dup_suspect, (unsigned long)n->st.echoes,
        (unsigned long)n->st.ws_reads, (unsigned long)n->st.ws_answers, n->pap.open ? "true" : "false", n->pap.connid,
        (unsigned long)n->pap.bytes);
    if (p < end) {
        at_printer_json(p, end - p);
        p += strlen(p);
    }
    if (p < end) p += snprintf(p, end - p, ",\"frames_recent\":[");
    json_ring(&p, end, sd->mem->fr, FR_LINES, sd->fr_head, sd->fr_n);
    xSemaphoreGive(s_lock);
    if (p > end) p = end;                          // a cut-short part never runs past buf
    snprintf(p, buf + cap - p, "]}");
}
