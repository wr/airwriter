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
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "lt_node.h"
#include "lt_tashtalk.h"

static const char *TAG = "lt";

#define LT_BAUD   1000000
#define JOB_CAP   (256 * 1024)      // PSRAM capture of the last PAP job
#define SPOOL     (16 * 1024)       // between the Mac and the printer; > LT_READ_MAX
#define EV_LINES  20
#define FR_LINES  10
#define LINE_LEN  100

static SemaphoreHandle_t s_lock;
static int        s_uart = -1, s_cts_pin = -1;
static int        s_running;
static lt_node_t  s_node;
static tt_rx_t    s_rx;
static char       s_obj[NBP_NAME_MAX + 1], s_type[NBP_NAME_MAX + 1];
static char       s_ev[EV_LINES][LINE_LEN], s_fr[FR_LINES][LINE_LEN];
static unsigned   s_ev_head, s_ev_n, s_fr_head, s_fr_n;
static unsigned   s_fcs_bad, s_errors, s_tx;
static uint8_t   *s_job;
static size_t     s_job_len;
static uint32_t   s_job_total;
static int        s_job_done;
static void     (*s_tap)(void *ctx, const uint8_t *f, size_t n);
static void      *s_tap_ctx;

// The print side. s_spool is single-producer (the LocalTalk task, under
// s_lock) and single-consumer (the print task), as a stream buffer must be.
// s_job_active runs from job_begin until the print task has handed the last
// byte to the printer and called end; the rest are set by the LocalTalk task.
static localtalk_printer_t s_printer;
static StreamBufferHandle_t s_spool;
static TaskHandle_t s_print_task;
static volatile int s_job_active, s_job_ended, s_job_normal, s_job_failed;
static uint32_t   s_jobs_printed, s_jobs_failed;

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
    static uint8_t cmd[1 + LLAP_MAX + 2];
    size_t cl = tt_send_cmd(cmd, sizeof cmd, f, n);
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

static int op_job_begin(void *ctx)
{
    (void)ctx;
    // The last job's tail still draining counts as busy: one job at a time.
    if (s_job_active) return 1;
    if (s_printer.begin && s_printer.begin(s_printer.ctx)) return 1;
    s_job_ended = s_job_normal = s_job_failed = 0;
    s_job_active = 1;
    s_job_len = 0;
    s_job_total = 0;
    s_job_done = 0;
    xTaskNotifyGive(s_print_task);
    return 0;
}

static void op_job_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (!s_job) s_job = heap_caps_malloc(JOB_CAP, MALLOC_CAP_SPIRAM);
    if (s_job) {
        size_t take = s_job_len + n <= JOB_CAP ? n : JOB_CAP - s_job_len;
        memcpy(s_job + s_job_len, d, take);
        s_job_len += take;
    }
    s_job_total += (uint32_t)n;
    // The node asked for no more than op_job_room had, so this never blocks.
    size_t put = xStreamBufferSend(s_spool, d, n, 0);
    if (put != n) ESP_LOGE(TAG, "spool overrun: %u of %u bytes", (unsigned)put, (unsigned)n);
}

static size_t op_job_room(void *ctx)
{
    (void)ctx;
    return xStreamBufferSpacesAvailable(s_spool);
}

static void op_job_end(void *ctx, int normal)
{
    (void)ctx;
    s_job_normal = normal;
    s_job_ended = 1;
    s_job_done = 1;
    xTaskNotifyGive(s_print_task);
}

// Drains the spool into the printer. A failed write aborts the job (the Mac
// is told with CloseConn); an abandoned job's rest is dropped, not printed.
static void print_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_spool, buf, sizeof buf, pdMS_TO_TICKS(100));
        if (n) {
            int drop = s_job_failed || (s_job_ended && !s_job_normal);
            if (!drop && s_printer.write && s_printer.write(s_printer.ctx, buf, n)) {
                s_job_failed = 1;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                lt_node_pap_abort(&s_node, now_ms());
                xSemaphoreGive(s_lock);
            }
            continue;
        }
        if (s_job_active && s_job_ended && xStreamBufferIsEmpty(s_spool)) {
            int ok = s_job_normal && !s_job_failed;
            if (s_printer.end) ok = s_printer.end(s_printer.ctx, ok);
            if (ok) s_jobs_printed++; else s_jobs_failed++;
            s_job_active = 0;
            continue;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
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
        lt_node_tick(&s_node, now_ms());
        int online = s_node.phase == LT_ONLINE && s_seen_phase != LT_ONLINE;
        int lookup = s_node.st.replies != s_seen_replies;
        int fresh = 0;
        for (size_t k = 0; k < sizeof s_seen_heard; k++) {
            if (s_node.heard[k] & ~s_seen_heard[k]) fresh = 1;
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

esp_err_t localtalk_start(const localtalk_cfg_t *cfg)
{
    if (s_running) return ESP_OK;
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
    s_uart = cfg->uart_num;
    s_cts_pin = cfg->cts_pin;
    ESP_ERROR_CHECK(uart_param_config(s_uart, &uc));
    ESP_ERROR_CHECK(uart_set_pin(s_uart, cfg->tx_pin, cfg->rx_pin, UART_PIN_NO_CHANGE,
                                 cfg->cts_pin >= 0 ? cfg->cts_pin : UART_PIN_NO_CHANGE));

    s_lock = xSemaphoreCreateMutex();
    strncpy(s_obj, cfg->object, sizeof s_obj - 1);
    strncpy(s_type, cfg->type, sizeof s_type - 1);

    uint8_t hint = 0;
    nvs_handle_t h;
    if (nvs_open("lt", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "node", &hint);
        nvs_close(h);
    }

    // As tashrouter starts it: TashTalk may be halfway through a command from
    // before this boot (an OTA reboot mid-frame), and 1024 no-ops return it to
    // waiting for a command byte; then no optional features (the FCS is ours
    // to compute and check). lt_node_init clears the node bitmap.
    static const uint8_t nops[64];
    for (int i = 0; i < TT_SYNC_LEN / (int)sizeof nops; i++) uart_write_bytes(s_uart, nops, sizeof nops);
    static const uint8_t features[] = { TT_CMD_FEATURES, 0x00 };
    uart_write_bytes(s_uart, features, sizeof features);

    s_printer = cfg->printer;
    s_on_event = cfg->on_event;
    uint8_t *spool = heap_caps_malloc(SPOOL + 1, MALLOC_CAP_SPIRAM);
    if (!spool) spool = malloc(SPOOL + 1);              // a chip with no PSRAM
    static StaticStreamBuffer_t spool_cb;
    if (!spool) return ESP_ERR_NO_MEM;
    s_spool = xStreamBufferCreateStatic(SPOOL, 1, spool, &spool_cb);
    if (xTaskCreate(print_task, "lt_print", 4096, NULL, 3, &s_print_task) != pdPASS)
        return ESP_ERR_NO_MEM;

    tt_rx_init(&s_rx);
    lt_cfg_t nc = { .object = cfg->object, .type = cfg->type, .status = cfg->status,
                    .hint = hint, .seed = esp_random() | 1 };
    lt_ops_t ops = { .send = op_send, .claim = op_claim, .log = op_log,
                     .job_begin = op_job_begin, .job_data = op_job_data,
                     .job_room = op_job_room, .job_end = op_job_end };
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lt_node_init(&s_node, &nc, &ops, now_ms());
    xSemaphoreGive(s_lock);

    if (xTaskCreate(lt_task, "localtalk", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s_running = 1;
    ESP_LOGI(TAG, "LocalTalk node starting as %s:%s@* (UART%d, tx IO%d rx IO%d cts IO%d, hint %u)",
             cfg->object, cfg->type, s_uart, cfg->tx_pin, cfg->rx_pin, cfg->cts_pin, hint);
    return ESP_OK;
}

int localtalk_running(void) { return s_running; }

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
    *len = s_job_len;
    return s_job_len ? s_job : NULL;
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
    if (!s_running) {
        snprintf(buf, cap, "{\"on\":false}");
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
    p += snprintf(p, end - p,
        ",\"frames\":%lu,\"fcs_bad\":%u,\"errors\":%u,\"tx\":%u,\"for_us\":%lu,"
        "\"lookups\":%lu,\"replies\":%lu,\"statuses\":%lu,\"opens\":%lu,\"busy\":%lu,"
        "\"dup\":%lu,\"echoes\":%lu,\"mac_reads\":%lu,\"mac_answers\":%lu,\"pap\":{\"open\":%s,\"conn\":%u,\"bytes\":%lu},"
        "\"job\":{\"captured\":%u,\"total\":%lu,\"done\":%s},"
        "\"print\":{\"active\":%s,\"spooled\":%u,\"failed\":%s,\"printed\":%lu,\"aborted\":%lu},"
        "\"events\":[",
        (unsigned long)n->st.frames, s_fcs_bad, s_errors, s_tx, (unsigned long)n->st.for_us,
        (unsigned long)n->st.lookups, (unsigned long)n->st.replies,
        (unsigned long)n->st.statuses, (unsigned long)n->st.opens, (unsigned long)n->st.busy,
        (unsigned long)n->st.dup_suspect, (unsigned long)n->st.echoes,
        (unsigned long)n->st.ws_reads, (unsigned long)n->st.ws_answers, n->pap.open ? "true" : "false", n->pap.connid,
        (unsigned long)n->pap.bytes, (unsigned)s_job_len, (unsigned long)s_job_total,
        s_job_done ? "true" : "false", s_job_active ? "true" : "false",
        (unsigned)xStreamBufferBytesAvailable(s_spool), s_job_failed ? "true" : "false",
        (unsigned long)s_jobs_printed, (unsigned long)s_jobs_failed);
    json_ring(&p, end, s_ev, EV_LINES, s_ev_head, s_ev_n);
    if (p < end) p += snprintf(p, end - p, "],\"frames_recent\":[");
    json_ring(&p, end, s_fr, FR_LINES, s_fr_head, s_fr_n);
    xSemaphoreGive(s_lock);
    snprintf(p, buf + cap - p, "]}");
}
