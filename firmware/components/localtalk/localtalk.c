// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP glue for the LocalTalk node; see localtalk.h. One task owns the UART,
// the TashTalk decoder and the node; s_lock covers them against the web UI's
// reads and status updates from other tasks.
#include "localtalk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
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
#define EV_LINES  20
#define FR_LINES  10
#define LINE_LEN  100
#define JSON_MACS 8         // Macs in /api/lt: eight, named, keep it inside its 4.5 KB

static SemaphoreHandle_t s_lock;
static int        s_uart = -1, s_cts_pin = -1;
static int        s_running;
static volatile int s_quiet;        // W-1079: switched off on the settings page
static lt_cfg_t   s_nc;             // the node's set-up, for coming back on
static lt_ops_t   s_ops;
static char       s_obj[NBP_NAME_MAX + 1], s_type[NBP_NAME_MAX + 1];
static unsigned   s_ev_head, s_ev_n, s_fr_head, s_fr_n;

// W-1094: the node, the TashTalk decoder, the logs and the send buffer, ~10 KB,
// in PSRAM rather than .bss, where they were internal RAM that Wi-Fi and lwIP
// buffer in (docs/traps.md). Tasks alone touch them: the UART driver copies
// what uart_write_bytes is handed, and reads into the task's own buffer. Made
// by a constructor, before app_main, so every use below finds it.
typedef struct {
    lt_node_t node;
    tt_rx_t   rx;
    char      ev[EV_LINES][LINE_LEN];
    char      fr[FR_LINES][LINE_LEN];
    uint8_t   cmd[1 + LLAP_MAX + 2];        // op_send's, on the LocalTalk task
} lt_mem_t;

static lt_mem_t *s_mem;
#define s_node (s_mem->node)
#define s_rx   (s_mem->rx)
#define s_ev   (s_mem->ev)
#define s_fr   (s_mem->fr)

__attribute__((constructor)) static void lt_mem_make(void)
{
    s_mem = heap_caps_calloc(1, sizeof *s_mem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mem) s_mem = calloc(1, sizeof *s_mem);    // a board with no PSRAM
    if (!s_mem) abort();
}
static unsigned   s_fcs_bad, s_errors, s_tx;
static void     (*s_tap)(void *ctx, const uint8_t *f, size_t n);
// W-1082: answer as the ImageWriter; the PAP client riding the node; where
// LaserWriters' NBP answers go.
static int        s_serve = 1;
static pap_client_t *s_client;
static void     (*s_found)(int link, const nbp_tuple_t *t, const lt_route_t *r);
static void     (*s_name_check)(int link, const nbp_tuple_t *t);
static void      *s_tap_ctx;

// W-1049: the node's counters as last seen, to tell the panel what changed.
// The LocalTalk task alone touches these.
static void     (*s_on_event)(lt_event_t ev);
static lt_phase_t s_seen_phase;
static uint32_t   s_seen_replies;
static uint8_t    s_seen_heard[sizeof s_node.heard];

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
static void note_frame(const char *dir, const uint8_t *f, size_t n)
{
    char line[LINE_LEN];
    int k = snprintf(line, sizeof line, "%s %uB %u>%u %02x |", dir, (unsigned)n, f[1], f[0], f[2]);
    for (size_t i = 3; i < n && k < (int)sizeof line - 4; i++)
        k += snprintf(line + k, sizeof line - k, " %02x", f[i]);
    push(s_fr, FR_LINES, &s_fr_head, &s_fr_n, line);
}

// ------------------------------------------------------------- node ops
// All called on the LocalTalk task with s_lock held.

static void op_send(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    uint8_t *cmd = s_mem->cmd;
    size_t cl = tt_send_cmd(cmd, sizeof s_mem->cmd, f, n);
    if (!cl) return;
    uart_write_bytes(s_uart, cmd, cl);       // TashTalk's CTS paces the hardware FIFO
    s_tx++;
    if (f[2] != LLAP_ENQ) note_frame("tx", f, n);
}

static void op_claim(void *ctx, uint8_t node)
{
    (void)ctx;
    uint8_t cmd[33];
    uart_write_bytes(s_uart, cmd, tt_nodes_cmd(cmd, node));
    if (!node) return;
    nvs_handle_t h;
    if (nvs_open("lt", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "node", node);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void op_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
    push(s_ev, EV_LINES, &s_ev_head, &s_ev_n, line);
}

// The printer gave up mid-job (on the print task): tell the Mac.
static void cable_abort(void *ctx)
{
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_pap_abort(&s_node, now_ms());
    xSemaphoreGive(s_lock);
}

static const at_owner_t s_owner = { AT_LINK_CABLE, cable_abort, localtalk_note, NULL };

static void op_found(void *ctx, const nbp_tuple_t *t, const lt_route_t *r)
{
    (void)ctx;
    if (s_found) s_found(AT_LINK_CABLE, t, r);
}

static void op_name_check(void *ctx, const nbp_tuple_t *t)
{
    (void)ctx;
    if (s_name_check) s_name_check(AT_LINK_CABLE, t);
}

// The node's own connection prints on the ImageWriter; one on another
// registration is the relay's (W-1088).
static const at_relay_t *relay_conn(void) { return lt_node_pap_reg(&s_node) ? at_relay() : NULL; }

static int op_job_begin(void *ctx) { (void)ctx; return at_job_begin(&s_owner); }
static void op_job_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    const at_relay_t *r = relay_conn();
    if (r) r->data(d, n);
    else at_job_data(&s_owner, d, n);
}
static size_t op_job_room(void *ctx)
{
    (void)ctx;
    const at_relay_t *r = relay_conn();
    return r ? r->room() : at_job_room(&s_owner);
}
static void op_job_end(void *ctx, int normal)
{
    (void)ctx;
    const at_relay_t *r = relay_conn();
    if (r) r->end(normal);
    else at_job_end(&s_owner, normal);
}
static int op_pap_open(void *ctx, int reg, uint64_t now)
{
    (void)ctx;
    return at_relay() ? at_relay()->open(AT_LINK_CABLE, &s_node, reg, now) : 1;
}
static size_t op_reg_status(void *ctx, int reg, uint8_t *pstr, size_t cap)
{
    (void)ctx;
    return at_relay() ? at_relay()->status(AT_LINK_CABLE, &s_node, reg, pstr, cap) : 0;
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
    (void)ctx;
    if (ev != TT_FRAME) {
        s_errors++;
        return;
    }
    if (!llap_fcs_ok(f, n)) {
        s_fcs_bad++;
        return;
    }
    n -= 2;
    if (s_tap) s_tap(s_tap_ctx, f, n);
    if (s_quiet) return;
    // Frames to us, not the Chooser's broadcasts or other nodes' traffic.
    if (s_node.phase == LT_ONLINE && f[0] == s_node.node) note_frame("rx", f, n);
    lt_node_rx(&s_node, f, n, now_ms());
}

// ------------------------------------------------------------- task

static void lt_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        // Wait up to a tick for the first byte, then take whatever else is in.
        int n = uart_read_bytes(s_uart, buf, 1, 1);
        if (n > 0) {
            size_t more = 0;
            uart_get_buffered_data_len(s_uart, &more);
            if (more > sizeof buf - 1) more = sizeof buf - 1;
            if (more) {
                int m = uart_read_bytes(s_uart, buf + 1, more, 0);
                if (m > 0) n += m;
            }
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (n > 0) tt_rx_feed(&s_rx, buf, (size_t)n, on_frame, NULL);
        if (!s_quiet) lt_node_tick(&s_node, now_ms());
        int online = s_node.phase == LT_ONLINE && s_seen_phase != LT_ONLINE;
        int lookup = s_node.st.replies != s_seen_replies;
        int fresh = 0;
        for (size_t k = 0; k < sizeof s_seen_heard; k++) {
            if (s_node.heard[k] & ~s_node.printers[k] & ~s_seen_heard[k]) fresh = 1;
            s_seen_heard[k] = s_node.heard[k];
        }
        s_seen_phase = s_node.phase;
        s_seen_replies = s_node.st.replies;
        xSemaphoreGive(s_lock);
        // Outside the lock: the panel's side takes its own.
        if (s_on_event) {
            if (online) s_on_event(LT_EV_ONLINE);
            if (lookup) s_on_event(LT_EV_LOOKUP);
            if (fresh)  s_on_event(LT_EV_NEW_NODE);
        }
    }
}

static void tt_close(const localtalk_cfg_t *cfg)
{
    uart_driver_delete(cfg->uart_num);
    gpio_reset_pin(cfg->tx_pin);
    gpio_reset_pin(cfg->rx_pin);
    if (cfg->cts_pin >= 0) gpio_reset_pin(cfg->cts_pin);
}

// Opens UART2 to TashTalk and sends the start-up sync. As tashrouter starts
// it: TashTalk may be halfway through a command from before this boot (an
// OTA reboot mid-frame), and 1024 no-ops return it to waiting for a command
// byte; then no optional features (the FCS is ours to compute and check).
// W-1081: TashTalk takes the sync in about 11 ms. With U7 empty or not
// running TashTalk, CTS stays up and none of it goes; a node started then
// would claim an ID and look online with no chip. So ESP_ERR_NOT_FOUND gives
// UART2 and the PIC's pins back, free for ICSP.
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
    esp_err_t err = uart_driver_install(cfg->uart_num, 8192, 4096, 0, NULL, 0);
    if (err != ESP_OK) return err;
    ESP_ERROR_CHECK(uart_param_config(cfg->uart_num, &uc));
    ESP_ERROR_CHECK(uart_set_pin(cfg->uart_num, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE,
                                 cfg->cts_pin >= 0 ? cfg->cts_pin : UART_PIN_NO_CHANGE));

    static const uint8_t nops[64];
    for (int i = 0; i < TT_SYNC_LEN / (int)sizeof nops; i++)
        uart_write_bytes(cfg->uart_num, nops, sizeof nops);
    static const uint8_t features[] = { TT_CMD_FEATURES, 0x00 };
    uart_write_bytes(cfg->uart_num, features, sizeof features);

    if (uart_wait_tx_done(cfg->uart_num, pdMS_TO_TICKS(LT_ALIVE_MS)) != ESP_OK) {
        tt_close(cfg);
        ESP_LOGW(TAG, "TashTalk took none of the sync");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t localtalk_probe(const localtalk_cfg_t *cfg)
{
    if (s_running) return ESP_OK;
    esp_err_t err = tt_open(cfg);
    if (err == ESP_OK) tt_close(cfg);
    return err;
}

static void offers_put(void);

esp_err_t localtalk_start(const localtalk_cfg_t *cfg)
{
    if (s_running) return ESP_OK;
    esp_err_t err = tt_open(cfg);
    if (err != ESP_OK) return err;
    s_uart = cfg->uart_num;
    s_cts_pin = cfg->cts_pin;

    s_lock = xSemaphoreCreateMutex();
    strncpy(s_obj, cfg->object, sizeof s_obj - 1);
    strncpy(s_type, cfg->type, sizeof s_type - 1);

    uint8_t hint = 0;
    nvs_handle_t h;
    if (nvs_open("lt", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "node", &hint);
        nvs_close(h);
    }

    s_on_event = cfg->on_event;
    err = at_printer_init(&cfg->printer);
    if (err != ESP_OK) return err;

    tt_rx_init(&s_rx);
    // The Macs' names and the node's registrations in PSRAM: internal RAM is short.
    s_nc = (lt_cfg_t){ .object = cfg->object, .type = cfg->type, .status = cfg->status,
                       .hint = hint, .seed = esp_random() | 1,
                       .names = heap_caps_calloc(LT_SEEN_SLOTS, sizeof(lt_name_t), MALLOC_CAP_SPIRAM),
                       .regs = heap_caps_calloc(LT_REG_MAX, sizeof(lt_reg_t), MALLOC_CAP_SPIRAM) };
    s_ops = (lt_ops_t){ .send = op_send, .claim = op_claim, .log = op_log,
                        .job_begin = op_job_begin, .job_data = op_job_data,
                        .job_room = op_job_room, .job_end = op_job_end, .found = op_found,
                        .name_check = op_name_check,
                        .pap_open = op_pap_open, .reg_status = op_reg_status,
                        .reg_output = op_reg_output };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_init(&s_node, &s_nc, &s_ops, now_ms());   // clears TashTalk's node bitmap
    lt_node_set_serve(&s_node, s_serve);
    offers_put();
    lt_node_attach(&s_node, s_client);
    xSemaphoreGive(s_lock);

    if (xTaskCreate(lt_task, "localtalk", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_running = 1;
    ESP_LOGI(TAG, "LocalTalk node starting as %s:%s@* (UART%d, tx IO%d rx IO%d cts IO%d, hint %u)",
             cfg->object, cfg->type, s_uart, cfg->tx_pin, cfg->rx_pin, cfg->cts_pin, hint);
    return ESP_OK;
}

int localtalk_running(void) { return s_running; }
int localtalk_quiet(void) { return s_running && s_quiet; }
int localtalk_serving(void) { return s_running && !s_quiet && s_serve; }

void localtalk_set_quiet(int quiet)
{
    quiet = quiet ? 1 : 0;
    if (!s_running || quiet == s_quiet) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t now = now_ms();
    if (quiet) {
        if (s_node.pap.open) lt_node_pap_abort(&s_node, now);
        if (s_client) papc_link_down(s_client, now);
        op_claim(NULL, 0);                       // TashTalk answers for no node now
        s_quiet = 1;
        op_log(NULL, "the AppleTalk port is off");
    } else {
        uint8_t hint = s_node.node;
        nvs_handle_t h;
        if (nvs_open("lt", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u8(h, "node", &hint);
            nvs_close(h);
        }
        s_nc.hint = hint;
        s_nc.status = s_node.status;
        s_nc.seed = esp_random() | 1;
        lt_node_init(&s_node, &s_nc, &s_ops, now);
        lt_node_set_serve(&s_node, s_serve);     // a new node: its roles again
        offers_put();
        lt_node_attach(&s_node, s_client);
        s_quiet = 0;
        op_log(NULL, "the AppleTalk port is on");
    }
    xSemaphoreGive(s_lock);
}

// W-1088: the cable's offers, in PSRAM (internal RAM is Wi-Fi's), for each new node.
static lt_offers_t *s_offers;

static void offers_put(void)
{
    if (s_offers) lt_node_offer(&s_node, s_offers->own, s_offers->rel, s_offers->n_rel, s_offers->marker);
}

void localtalk_set_offers(const lt_offers_t *o)
{
    if (!s_offers) s_offers = heap_caps_calloc(1, sizeof *s_offers, MALLOC_CAP_SPIRAM);
    if (!s_offers) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *s_offers = *o;
    if (s_running) offers_put();
    if (s_lock) xSemaphoreGive(s_lock);
}

void localtalk_set_roles(int serve, int laser)
{
    if (!s_running) return;
    serve = serve ? 1 : 0;
    if (!serve && !laser) {
        localtalk_set_quiet(1);
        return;
    }
    if (s_quiet) localtalk_set_quiet(0);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_serve != serve) {
        // No ImageWriter here any more: a Mac printing to it is told.
        if (!serve && s_node.pap.open) lt_node_pap_abort(&s_node, now_ms());
        s_serve = serve;
        lt_node_set_serve(&s_node, serve);
        op_log(NULL, serve ? "answering as the ImageWriter" : "running for a LaserWriter only");
    }
    xSemaphoreGive(s_lock);
}

void localtalk_lookup(const char *type)
{
    if (!s_running || s_quiet) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_lookup(&s_node, type, now_ms());
    xSemaphoreGive(s_lock);
}

void localtalk_set_found(void (*found)(int link, const nbp_tuple_t *t, const lt_route_t *r))
{
    s_found = found;
}

void localtalk_set_name_check(void (*check)(int link, const nbp_tuple_t *t))
{
    s_name_check = check;
}

void localtalk_attach(pap_client_t *c)
{
    if (!s_running) {
        s_client = c;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_client = c;
    lt_node_attach(&s_node, c);
    xSemaphoreGive(s_lock);
}

int localtalk_locked(void (*fn)(void *arg), void *arg)
{
    if (!s_running) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    fn(arg);
    xSemaphoreGive(s_lock);
    return 0;
}

void localtalk_set_status(uint16_t status)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_set_status(&s_node, status);
    xSemaphoreGive(s_lock);
}

void localtalk_note(const char *line)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    op_log(NULL, line);
    xSemaphoreGive(s_lock);
}

void localtalk_echo(uint8_t node)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_echo(&s_node, node, now_ms());
    xSemaphoreGive(s_lock);
}

const uint8_t *localtalk_job(size_t *len)
{
    return at_job_bytes(len);
}

void localtalk_set_tap(void (*tap)(void *ctx, const uint8_t *f, size_t n), void *ctx)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_tap = tap;
    s_tap_ctx = ctx;
    xSemaphoreGive(s_lock);
}

void localtalk_send_raw(const uint8_t *f, size_t n)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    op_send(NULL, f, n);
    xSemaphoreGive(s_lock);
}

int localtalk_tt_alive(int wait_ms)
{
    if (!s_running) return 0;
    TickType_t t = pdMS_TO_TICKS(wait_ms);
    return uart_wait_tx_done(s_uart, t ? t : 1) == ESP_OK;
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

void localtalk_json(char *buf, size_t cap)
{
    // W-1082: the AppleTalk Port card's "on" is the ImageWriter answering on
    // the cable, not a node running for a LaserWriter alone (review C1).
    if (!s_running || s_quiet || !s_serve) {
        snprintf(buf, cap, "{\"on\":false,\"quiet\":%s,\"laser\":%s}",
                 s_running && (s_quiet || !s_serve) ? "true" : "false",
                 s_running && !s_quiet && !s_serve ? "true" : "false");
        return;
    }
    char *p = buf, *end = buf + cap - 8;
    size_t tx_free = 0;
    uart_get_tx_buffer_free_size(s_uart, &tx_free);
    int cts = s_cts_pin >= 0 ? gpio_get_level(s_cts_pin) : -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const lt_node_t *n = &s_node;
    p += snprintf(p, end - p,
        "{\"on\":true,\"phase\":\"%s\",\"node\":%u,\"net\":%u,\"status\":\"0x%04x\","
        "\"object\":",
        n->phase == LT_ONLINE ? "online" : "acquiring", n->node, n->net,
        n->status);
    json_str(&p, end, s_obj);
    p += snprintf(p, end - p, ",\"type\":");
    json_str(&p, end, s_type);
    p += snprintf(p, end - p, ",\"cts\":%d,\"tx_free\":%u,\"heard\":[", cts, (unsigned)tx_free);
    for (int id = 1, first = 1; id < 255 && p < end; id++) {
        if (!(n->heard[id / 8] & (1u << (id % 8)))) continue;
        p += snprintf(p, end - p, "%s%d", first ? "" : ",", id);
        first = 0;
    }
    if (p < end) *p++ = ']';
    // W-1079: the Macs heard in the last 10 minutes, for the settings page,
    // with the Macintosh name of each that gave one.
    lt_seen_t seen[JSON_MACS];
    size_t ns = lt_node_seen(n, now_ms(), LT_SEEN_MS, seen, JSON_MACS);
    if (p < end) p += snprintf(p, end - p, ",\"macs\":[");
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
    if (p < end) p += snprintf(p, end - p,
        ",\"frames\":%lu,\"fcs_bad\":%u,\"errors\":%u,\"tx\":%u,\"for_us\":%lu,"
        "\"lookups\":%lu,\"replies\":%lu,\"statuses\":%lu,\"opens\":%lu,\"busy\":%lu,"
        "\"dup\":%lu,\"echoes\":%lu,\"mac_reads\":%lu,\"mac_answers\":%lu,\"pap\":{\"open\":%s,\"conn\":%u,\"bytes\":%lu},",
        (unsigned long)n->st.frames, s_fcs_bad, s_errors, s_tx, (unsigned long)n->st.for_us,
        (unsigned long)n->st.lookups, (unsigned long)n->st.replies,
        (unsigned long)n->st.statuses, (unsigned long)n->st.opens, (unsigned long)n->st.busy,
        (unsigned long)n->st.dup_suspect, (unsigned long)n->st.echoes,
        (unsigned long)n->st.ws_reads, (unsigned long)n->st.ws_answers, n->pap.open ? "true" : "false", n->pap.connid,
        (unsigned long)n->pap.bytes);
    if (p < end) {
        at_printer_json(p, end - p);
        p += strlen(p);
    }
    if (p < end) p += snprintf(p, end - p, ",\"events\":[");
    json_ring(&p, end, s_ev, EV_LINES, s_ev_head, s_ev_n);
    if (p < end) p += snprintf(p, end - p, "],\"frames_recent\":[");
    json_ring(&p, end, s_fr, FR_LINES, s_fr_head, s_fr_n);
    xSemaphoreGive(s_lock);
    if (p > end) p = end;                          // a cut-short part never runs past buf
    snprintf(p, buf + cap - p, "]}");
}
