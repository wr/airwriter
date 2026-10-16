// SPDX-License-Identifier: GPL-3.0-or-later
// The cable tester, ESP side; see cable_test.h.
#include "cable_test.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "localtalk.h"
#include "lt_proto.h"
#include "lt_tashtalk.h"
#include "serial_out.h"

static const char *TAG = "cable";

#define RES_HZ       (40 * 1000 * 1000)   // 86.8 ticks a half cell
#define LT_BAUD      1000000
#define TRIES        3
// TashTalk waits out a 400 us interdialog gap plus up to 1.5 ms of backoff,
// then sends a 5-byte control frame in about 0.5 ms.
#define BACK_WAIT_MS 30
#define FWD_WAIT_MS  20
#define RX_SYMS      192                  // all four RX blocks: a frame is ~65
#define TX_SYMS      (CT_RUNS_MAX / 2 + 1)

static cable_test_cfg_t  s_cfg;
static QueueHandle_t     s_rx_q;
static rmt_symbol_word_t s_rx_syms[RX_SYMS];
static uint32_t          s_ticks[2 * RX_SYMS];
static uint8_t           s_runs[CT_RUNS_MAX];
static uint16_t          s_run_ticks[CT_RUNS_MAX];
static rmt_symbol_word_t s_tx_syms[TX_SYMS];

// J4 -> J2: a lapENQ for node 0, which no node answers. TashTalk sends a
// control frame straight away, without the RTS a data frame would need.
static const uint8_t BACK[] = { 0x00, 0x00, LLAP_ENQ };
// J2 -> J4: the same with four random bytes after it. The node ignores both
// (it is not node 0, and an ENQ marks no node as heard).
static uint8_t       s_fwd[LLAP_HDR + 4];
static volatile int  s_fwd_seen;
static volatile int  s_lt_owned;      // UART2 is ours right now; see cable_test_lt_busy

// Evidence reported with each result (dbg in the JSON): what each leg saw.
static struct {
    int     caps, syms[6];           // J4->J2: RMT captures and their symbol counts
    int     tt_frames, tt_bad, tt_err;   // J2->J4: what TashTalk relayed, own link only
    int     nfirst;
    uint8_t first[12];               // the first relayed frame's bytes
    int     cts;                     // TashTalk's CTS when the ENQ went out
    int     reset;                   // 1: TashTalk was reset after a fail; 2: and then passed
} s_dbg;

static TickType_t ms_ticks(int ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return t ? t : 1;
}

// ------------------------------------------------------------- the TashTalk link

typedef struct {
    int     owned;          // 1: UART2 is ours for the test; 0: the node's, through its tap
    tt_rx_t rx;
} link_t;

static void on_tap(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (n == sizeof s_fwd && !memcmp(f, s_fwd, n)) s_fwd_seen = 1;
}

static void on_tt(void *ctx, tt_event_t ev, const uint8_t *f, size_t n)
{
    if (ev != TT_FRAME) { s_dbg.tt_err++; return; }
    if (!s_dbg.nfirst) {
        s_dbg.nfirst = n < sizeof s_dbg.first ? (int)n : (int)sizeof s_dbg.first;
        memcpy(s_dbg.first, f, (size_t)s_dbg.nfirst);
    }
    if (!llap_fcs_ok(f, n)) { s_dbg.tt_bad++; return; }
    s_dbg.tt_frames++;
    on_tap(ctx, f, n - 2);
}

static void tt_sync(void);

static esp_err_t link_open(link_t *lk)
{
    memset(lk, 0, sizeof *lk);
    if (localtalk_running()) {
        localtalk_set_tap(on_tap, NULL);
        return ESP_OK;
    }
    // TashTalk runs whenever the board does (the PIC's reset idles released),
    // so with the node off it is only a UART away. As localtalk_start brings
    // it up: no-ops to finish any half-sent command, no features, and here
    // answering for no node at all.
    uart_config_t uc = {
        .baud_rate  = LT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = s_cfg.lt_cts >= 0 ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // A TX ring big enough that no write here blocks: a PIC held in reset or
    // never flashed leaves CTS high, and the drain's timeout catches that
    // instead of a write hanging with the job lock held.
    esp_err_t err = uart_driver_install(s_cfg.lt_uart, 1024, 2048, 0, NULL, 0);
    if (err != ESP_OK) return err;
    lk->owned = 1;
    s_lt_owned = 1;
    uart_param_config(s_cfg.lt_uart, &uc);
    uart_set_pin(s_cfg.lt_uart, s_cfg.lt_tx, s_cfg.lt_rx, UART_PIN_NO_CHANGE,
                 s_cfg.lt_cts >= 0 ? s_cfg.lt_cts : UART_PIN_NO_CHANGE);
    tt_sync();
    err = uart_wait_tx_done(s_cfg.lt_uart, ms_ticks(100));     // 11 ms of bytes at 1 Mbaud
    uart_flush_input(s_cfg.lt_uart);
    tt_rx_init(&lk->rx);
    return err == ESP_OK ? ESP_OK : ESP_ERR_TIMEOUT;
}

// Holds TashTalk in reset for 2 ms and lets it run again (its pull-up on
// MCLR; a 64 ms power-up timer), then syncs it as link_open does. Only on our
// own link: with the node off nothing else is talking to TashTalk.
static void tt_reset(void)
{
    gpio_set_level(s_cfg.lt_mclr, 0);
    gpio_set_direction(s_cfg.lt_mclr, GPIO_MODE_OUTPUT);
    vTaskDelay(ms_ticks(2));
    gpio_set_direction(s_cfg.lt_mclr, GPIO_MODE_INPUT);
    vTaskDelay(ms_ticks(150));
    tt_sync();
    uart_wait_tx_done(s_cfg.lt_uart, ms_ticks(100));
    uart_flush_input(s_cfg.lt_uart);
}

static void tt_sync(void)
{
    static const uint8_t nops[64];
    for (int i = 0; i < TT_SYNC_LEN / (int)sizeof nops; i++)
        uart_write_bytes(s_cfg.lt_uart, nops, sizeof nops);
    static const uint8_t features[] = { TT_CMD_FEATURES, 0x00 };
    uart_write_bytes(s_cfg.lt_uart, features, sizeof features);
    uint8_t nodes[33];
    uart_write_bytes(s_cfg.lt_uart, nodes, tt_nodes_cmd(nodes, 0));
}

static void link_send(link_t *lk, const uint8_t *f, size_t n)
{
    if (!lk->owned) {
        localtalk_send_raw(f, n);
        return;
    }
    uint8_t cmd[1 + CT_FRAME_MAX + 2];
    size_t cl = tt_send_cmd(cmd, sizeof cmd, f, n);
    if (cl) uart_write_bytes(s_cfg.lt_uart, cmd, cl);
}

// Takes in what TashTalk relays for up to ms (the node's tap does this on
// its own task when the link is the node's).
static void link_poll(link_t *lk, int ms)
{
    if (!lk->owned) {
        vTaskDelay(ms_ticks(ms));
        return;
    }
    uint8_t buf[128];
    int n = uart_read_bytes(s_cfg.lt_uart, buf, sizeof buf, ms_ticks(ms));
    if (n > 0) tt_rx_feed(&lk->rx, buf, (size_t)n, on_tt, NULL);
}

static void link_close(link_t *lk)
{
    if (!lk->owned) {
        localtalk_set_tap(NULL, NULL);
        return;
    }
    uart_driver_delete(s_cfg.lt_uart);
    // The driver leaves TashTalk's RX floating, and TashTalk reads noise as
    // commands: back to the boot clamp, high, the UART idle level.
    gpio_config_t io = { .pin_bit_mask = 1ULL << s_cfg.lt_tx, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(s_cfg.lt_tx, 1);
    gpio_config(&io);
    s_lt_owned = 0;
}

int cable_test_lt_busy(void) { return s_lt_owned; }

// ------------------------------------------------------------- J4 -> J2

static bool on_rx_done(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t *ed, void *ctx)
{
    (void)ch;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    size_t n = ed->num_symbols;
    xQueueSendFromISR(s_rx_q, &n, &woken);
    return woken == pdTRUE;
}

static size_t syms_to_ticks(const rmt_symbol_word_t *s, size_t n, uint32_t *out)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (!s[i].duration0) break;
        out[k++] = s[i].duration0;
        if (!s[i].duration1) break;
        out[k++] = s[i].duration1;
    }
    return k;
}

static int back_leg(link_t *lk, esp_err_t *err)
{
    rmt_channel_handle_t rx = NULL;
    rmt_rx_channel_config_t rc = {
        .gpio_num = s_cfg.rx_pin, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RES_HZ,
    };
    // As many of the four 48-symbol RX blocks as are free; one holds a frame
    // with a little noise around it.
    for (rc.mem_block_symbols = RX_SYMS; rc.mem_block_symbols >= 48; rc.mem_block_symbols /= 2)
        if ((*err = rmt_new_rx_channel(&rc, &rx)) == ESP_OK) break;
    if (*err != ESP_OK) return 0;
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = on_rx_done };
    rmt_rx_register_event_callbacks(rx, &cbs, NULL);
    // Up to 2.5 half cells is a run; the gap after TashTalk's sync pulse is
    // ~20 us (159 cycles at 8 MHz), so a capture ends only once the line has
    // been quiet for 60.
    rmt_receive_config_t rcv = { .signal_range_min_ns = 200, .signal_range_max_ns = 60000 };

    int ok = 0;
    for (int t = 0; t < TRIES && !ok; t++) {
        rmt_enable(rx);
        xQueueReset(s_rx_q);
        rmt_receive(rx, s_rx_syms, sizeof s_rx_syms, &rcv);
        if (s_cfg.lt_cts >= 0) s_dbg.cts = gpio_get_level(s_cfg.lt_cts);
        link_send(lk, BACK, sizeof BACK);
        int64_t until = esp_timer_get_time() + BACK_WAIT_MS * 1000;
        while (!ok) {
            int64_t left_ms = (until - esp_timer_get_time()) / 1000;
            size_t n;
            if (left_ms <= 0 || xQueueReceive(s_rx_q, &n, ms_ticks((int)left_ms)) != pdTRUE) break;
            size_t k = syms_to_ticks(s_rx_syms, n, s_ticks);
            if (s_dbg.caps < 6) s_dbg.syms[s_dbg.caps] = (int)n;
            s_dbg.caps++;
            ok = ct_capture_has(s_ticks, k, RES_HZ, BACK, sizeof BACK);
            // Noise on the open pair ended that capture early: listen again.
            if (!ok) rmt_receive(rx, s_rx_syms, sizeof s_rx_syms, &rcv);
        }
        rmt_disable(rx);                        // drops a receive still waiting
        if (lk->owned) uart_flush_input(s_cfg.lt_uart);
    }
    rmt_del_channel(rx);
    return ok;
}

// ------------------------------------------------------------- J2 -> J4

static int fwd_leg(link_t *lk, esp_err_t *err)
{
    uint32_t nonce = esp_random();
    s_fwd[0] = 0x00;
    s_fwd[1] = 0x00;
    s_fwd[2] = LLAP_ENQ;
    memcpy(s_fwd + LLAP_HDR, &nonce, 4);

    // Runs alternate from 1, the level away from idle; RMT idles at 0 and the
    // output is inverted unless the UART's is, so the pin idles where the
    // UART left it.
    size_t nr = ct_fm0_encode(s_fwd, sizeof s_fwd, s_runs, sizeof s_runs);
    ct_runs_to_ticks(s_runs, nr, RES_HZ, s_run_ticks);
    size_t ns = 0;
    for (size_t i = 0; i < nr; i += 2) {
        uint16_t d1 = i + 1 < nr ? s_run_ticks[i + 1] : s_run_ticks[i];   // odd: idle after
        s_tx_syms[ns++] = (rmt_symbol_word_t){ .duration0 = s_run_ticks[i], .level0 = 1,
                                               .duration1 = d1, .level1 = 0 };
    }

    // UART1 holds the pin's reservation; RMT would warn about a conflict.
    esp_gpio_revoke(BIT64(s_cfg.tx_pin));
    rmt_channel_handle_t tx = NULL;
    rmt_tx_channel_config_t tc = {
        .gpio_num = s_cfg.tx_pin, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RES_HZ, .mem_block_symbols = 48, .trans_queue_depth = 1,
        .flags.invert_out = !s_cfg.invert_tx,
    };
    rmt_encoder_handle_t enc = NULL;
    rmt_copy_encoder_config_t ec = {};
    *err = rmt_new_tx_channel(&tc, &tx);
    if (*err == ESP_OK) *err = rmt_new_copy_encoder(&ec, &enc);
    if (*err != ESP_OK) {
        if (tx) rmt_del_channel(tx);
        return 0;
    }
    rmt_enable(tx);
    rmt_transmit_config_t tcfg = { .loop_count = 0, .flags.eot_level = 0 };

    int ok = 0;
    s_fwd_seen = 0;
    for (int t = 0; t < TRIES && !ok; t++) {
        rmt_transmit(tx, enc, s_tx_syms, ns * sizeof s_tx_syms[0], &tcfg);
        rmt_tx_wait_all_done(tx, 20);
        for (int waited = 0; waited < FWD_WAIT_MS && !s_fwd_seen; waited += 10)
            link_poll(lk, 10);
        ok = s_fwd_seen;
    }
    rmt_disable(tx);
    rmt_del_channel(tx);
    rmt_del_encoder(enc);
    return ok;
}

// ------------------------------------------------------------- the test

void cable_test_init(const cable_test_cfg_t *cfg)
{
    s_cfg = *cfg;
    if (!s_rx_q) s_rx_q = xQueueCreate(4, sizeof(size_t));
}

void cable_test_run(cable_result_t *r, int both)
{
    memset(r, 0, sizeof *r);
    memset(&s_dbg, 0, sizeof s_dbg);
    int64_t t0 = esp_timer_get_time();
    link_t lk;
    r->err = link_open(&lk);
    if (r->err != ESP_OK && lk.owned) link_close(&lk);     // TashTalk took no bytes
    if (r->err == ESP_OK) {
        serial_out_lend(1);
        r->back_ok = back_leg(&lk, &r->err);
        // On the bench, before the node had ever run that boot, TashTalk once
        // sent and relayed nothing over our own link while its CTS said it
        // was alive; the cause was never found (W-1055). A fail on our own
        // link gets one more try from a reset TashTalk, and the log says if
        // that is what passed.
        if (!r->back_ok && lk.owned && r->err == ESP_OK && s_cfg.lt_mclr >= 0) {
            tt_reset();
            r->back_ok = back_leg(&lk, &r->err);
            s_dbg.reset = 1 + r->back_ok;
            if (r->back_ok) ESP_LOGW(TAG, "TashTalk answered only after a reset");
        }
        if (r->back_ok || both) r->fwd_ok = fwd_leg(&lk, &r->err);
        serial_out_lend(0);
        link_close(&lk);
    }
    r->verdict = ct_verdict(r->back_ok, r->fwd_ok);
    if (r->err != ESP_OK)
        ESP_LOGE(TAG, "could not run: %s", esp_err_to_name(r->err));
    ESP_LOGI(TAG, "J4->J2 %s, J2->J4 %s: %s (%lld ms, %s link)",
             r->back_ok ? "ok" : "nothing", r->fwd_ok ? "ok" : r->back_ok ? "nothing" : "not tried",
             ct_verdict_name(r->verdict), (esp_timer_get_time() - t0) / 1000,
             lk.owned ? "own" : "node's");
}

void cable_result_json(const cable_result_t *r, char *buf, size_t cap)
{
    int k;
    if (r->err != ESP_OK)
        k = snprintf(buf, cap, "{\"ok\":false,\"error\":\"%s\"", esp_err_to_name(r->err));
    else
        k = snprintf(buf, cap, "{\"ok\":true,\"verdict\":\"%s\",\"back\":%s,\"fwd\":%s,\"text\":\"%s\"",
                     ct_verdict_name(r->verdict), r->back_ok ? "true" : "false",
                     r->fwd_ok ? "true" : "false", ct_verdict_text(r->verdict));
    if (k < 0 || (size_t)k >= cap) return;
    k += snprintf(buf + k, cap - k, ",\"dbg\":{\"caps\":%d,\"syms\":[%d,%d,%d,%d,%d,%d],\"cts\":%d,"
                  "\"tt\":[%d,%d,%d],\"reset\":%d,\"first\":\"", s_dbg.caps, s_dbg.syms[0], s_dbg.syms[1],
                  s_dbg.syms[2], s_dbg.syms[3], s_dbg.syms[4], s_dbg.syms[5], s_dbg.cts,
                  s_dbg.tt_frames, s_dbg.tt_bad, s_dbg.tt_err, s_dbg.reset);
    for (int i = 0; i < s_dbg.nfirst && (size_t)k < cap; i++)
        k += snprintf(buf + k, cap - k, "%02x", s_dbg.first[i]);
    if ((size_t)k < cap) snprintf(buf + k, cap - k, "\"}}");
}
