// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_out.h"
#include "serial_writer.h"

#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "hal/uart_ll.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "serial";

// No TX ring, deliberately. serial_out_write feeds the hardware FIFO directly
// through uart_tx_chars, so writes are non-blocking by construction and a
// failed job leaves at most one FIFO (128 bytes) of residue to purge -- a
// ring would reintroduce both problems (uart_write_bytes has no timeout, and
// uart_get_tx_buffer_free_size only tracks the ring item in service).
#define TX_RING      0
#define RX_RING      1024   // the loopback self-check, and the printer's self-ID (W-1064)
#define WATCH_PERIOD pdMS_TO_TICKS(100)

// The clock on HSKi (W-1102). PCNT's counter is 16 bits: a 533 kHz clock is 26,650 edges in
// a 50 ms window, and the unit accumulates past its limit, so a window stretched by a busy
// core still counts true.
#define CLOCK_WINDOW pdMS_TO_TICKS(50)
#define CLOCK_HZ_MIN 100000
#define CLOCK_LIMIT  32767
#define EVENT_QUEUE  8

static serial_out_cfg_t s_cfg;
static unsigned s_stall_limit;   // poll periods; see serial_writer.h
static volatile int s_abort;     // see serial_out_set_abort()
static volatile size_t   s_lb_bytes;
static volatile uint32_t s_lb_crc;
static pcnt_unit_handle_t s_pcnt;      // rising edges on cts_pin; NULL with no CTS line
static volatile int s_clocked, s_clock_hz;
static QueueHandle_t s_events;         // the UART driver's, read only by serial_out_break_take

// The port's link right now (serial_out_set_link): the ImageWriter's, 9600 with CTS, until a
// StyleWriter's is asked for. Read and written under the job lock, like the calls that use it.
static int s_link_baud;
static int s_link_cts;
// Longest serial_out_set_link waits for the transmitter before it discards what is left:
// 128 bytes of FIFO are 133 ms at 9600.
#define LINK_DRAIN_MS 300

static uint32_t crc32_up(uint32_t crc, const uint8_t *buf, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// HSKo is driven positive (RS-232 "space") when the printer can accept data.
// RS-232 receivers invert, so the SP3232 hands the ESP32 a logic LOW for an
// asserted line -- which is what the UART peripheral's active-low CTS wants.
// invert_cts flips both this check and the peripheral's CTS sense, for the
// case where the real printer turns out to idle the other way.
static int clear_to_send(void)
{
    if (s_cfg.cts_pin < 0) return 1;
    int level = gpio_get_level(s_cfg.cts_pin);
    return s_cfg.invert_cts ? level == 1 : level == 0;
}

// The edge counter on cts_pin. Its channel goes through the GPIO matrix, so the pad stays the
// UART's CTS input.
static void clock_counter_init(void)
{
    pcnt_unit_config_t uc = { .low_limit = -1, .high_limit = CLOCK_LIMIT, .flags.accum_count = 1 };
    pcnt_chan_config_t cc = { .edge_gpio_num = s_cfg.cts_pin, .level_gpio_num = -1 };
    pcnt_channel_handle_t ch = NULL;
    if (pcnt_new_unit(&uc, &s_pcnt) != ESP_OK) { s_pcnt = NULL; return; }
    if (pcnt_new_channel(s_pcnt, &cc, &ch) != ESP_OK ||
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
        pcnt_unit_add_watch_point(s_pcnt, CLOCK_LIMIT) != ESP_OK ||
        pcnt_unit_enable(s_pcnt) != ESP_OK || pcnt_unit_start(s_pcnt) != ESP_OK) {
        ESP_LOGW(TAG, "no edge counter on the printer's handshake line");
        if (ch) pcnt_del_channel(ch);
        pcnt_del_unit(s_pcnt);
        s_pcnt = NULL;
    }
}

// The clock's rate over one window, in Hz; -1 with no counter.
static int clock_window(void)
{
    if (!s_pcnt) return -1;
    int n = 0;
    pcnt_unit_clear_count(s_pcnt);
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(CLOCK_WINDOW);
    pcnt_unit_get_count(s_pcnt, &n);
    int64_t dt = esp_timer_get_time() - t0;
    return dt > 0 && n > 0 ? (int)((int64_t)n * 1000000 / dt) : 0;
}

// Reports HSKo edges. Mid-job the line toggles on every printed line, so the
// callback must stay cheap; printer_state does the judging on its own clock.
// W-1102: each 100 ms also counts the line's edges for 50 ms. A clock (a QuickDraw
// LaserWriter switched on) is reported by on_clock, and while it runs the level is not.
static void line_task(void *arg)
{
    (void)arg;
    int last = -1, runs = 0;
    for (;;) {
        int hz = clock_window();
        int fast = hz >= CLOCK_HZ_MIN;
        if (fast != s_clocked) {
            if (++runs >= 2) {                      // two windows running decide it
                runs = 0;
                s_clocked = fast;
                s_clock_hz = hz;
                if (!fast) last = -1;               // the level is reported afresh
                if (s_cfg.on_clock) s_cfg.on_clock(s_cfg.on_line_ctx, fast, hz);
            }
        } else {
            runs = 0;
            if (fast) s_clock_hz = hz;
        }
        if (!s_clocked && !fast) {
            int now = clear_to_send();
            if (now != last) {
                last = now;
                ESP_LOGD(TAG, "printer %s", now ? "ready" : "not ready");
                if (s_cfg.on_line) s_cfg.on_line(s_cfg.on_line_ctx, now);
            }
        }
        vTaskDelay(s_pcnt ? WATCH_PERIOD - CLOCK_WINDOW : WATCH_PERIOD);
    }
}

// Drains whatever loopback returns so the RX ring never fills and stalls the
// peripheral. Only runs when loopback is on.
static void loopback_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(s_cfg.uart_num, buf, sizeof buf, pdMS_TO_TICKS(200));
        if (n > 0) {
            s_lb_crc = crc32_up(s_lb_crc, buf, (size_t)n);
            s_lb_bytes += (size_t)n;
        }
    }
}

esp_err_t serial_out_init(const serial_out_cfg_t *cfg)
{
    s_cfg = *cfg;
    if (!s_cfg.baud) s_cfg.baud = 9600;
    s_stall_limit = (unsigned)(s_cfg.stall_abort_s > 0 ? s_cfg.stall_abort_s : 600)
                    * (1000u / 100u);   // WATCH_PERIOD is 100 ms

    uart_config_t uc = {
        .baud_rate = s_cfg.baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        // CTS only. The printer never asks us to stop receiving, and RTS is
        // not wired to anything.
        .flow_ctrl = (s_cfg.cts_pin >= 0 && !s_cfg.loopback)
                       ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // The event queue is for the printer's BREAK alone (W-1102, serial_out_break_take).
    esp_err_t err = uart_driver_install(s_cfg.uart_num, RX_RING, TX_RING, EVENT_QUEUE,
                                        &s_events, 0);
    if (err != ESP_OK) return err;

    err = uart_param_config(s_cfg.uart_num, &uc);
    if (err != ESP_OK) return err;

    err = uart_set_pin(s_cfg.uart_num, s_cfg.tx_pin, s_cfg.rx_pin,
                       UART_PIN_NO_CHANGE,
                       (s_cfg.cts_pin >= 0 && !s_cfg.loopback)
                         ? s_cfg.cts_pin : UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    uint32_t inv = 0;
    if (s_cfg.invert_tx) inv |= UART_SIGNAL_TXD_INV;
    if (s_cfg.invert_rx) inv |= UART_SIGNAL_RXD_INV;
    if (s_cfg.invert_cts && s_cfg.cts_pin >= 0 && !s_cfg.loopback)
        inv |= UART_SIGNAL_CTS_INV;
    if (inv) {
        err = uart_set_line_inverse(s_cfg.uart_num, inv);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "line inversion: tx %s, rx %s, cts %s",
                 s_cfg.invert_tx ? "inverted" : "normal",
                 s_cfg.invert_rx ? "inverted" : "normal",
                 s_cfg.invert_cts ? "inverted" : "normal");
    }

    if (s_cfg.loopback) {
        err = uart_set_loop_back(s_cfg.uart_num, true);
        if (err != ESP_OK) return err;
        xTaskCreate(loopback_task, "ser_lb", 3072, NULL, 6, NULL);
        ESP_LOGW(TAG, "loopback on: TX feeds RX inside the peripheral, nothing "
                      "reaches a printer");
    } else if (s_cfg.cts_pin >= 0) {
        clock_counter_init();
        xTaskCreate(line_task, "ser_line", 3072, NULL, 3, NULL);
    }
    // No line to watch: say so once, so the state is not left unknown.
    if (s_cfg.loopback && s_cfg.on_line)         s_cfg.on_line(s_cfg.on_line_ctx, 1);
    else if (s_cfg.cts_pin < 0 && s_cfg.on_line) s_cfg.on_line(s_cfg.on_line_ctx, 0);

    s_link_baud = s_cfg.baud;
    s_link_cts  = s_cfg.cts_pin >= 0 && !s_cfg.loopback;

    ESP_LOGI(TAG, "uart%d at %d baud, tx %d rx %d cts %d",
             s_cfg.uart_num, s_cfg.baud, s_cfg.tx_pin, s_cfg.rx_pin, s_cfg.cts_pin);
    return ESP_OK;
}

// The loop itself lives in serial_writer.c, pure C so the host tests cover
// it. These ops bind it to the driver and the watchdog. The rule that
// matters: every op is non-blocking. uart_tx_chars fills what the hardware
// FIFO can take and reports the count -- unlike uart_write_bytes, which has
// no timeout and would park the rip task inside the driver forever the
// moment a dead printer's CTS stalled the drain.
static int ops_tx_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    return uart_tx_chars(s_cfg.uart_num, (const char *)buf, n);
}

static int ops_aborted(void *ctx)
{
    (void)ctx;
    return s_abort;
}

static void ops_delay(void *ctx)
{
    (void)ctx;
    vTaskDelay(WATCH_PERIOD);
}

esp_err_t serial_out_write(const uint8_t *buf, size_t n)
{
    const serial_writer_ops_t ops = {
        .tx_write    = ops_tx_write,
        .delay       = ops_delay,
        .aborted     = ops_aborted,
        .stall_limit = s_stall_limit,
    };
    return serial_writer_write(&ops, buf, n) == 0 ? ESP_OK : ESP_FAIL;
}

void serial_out_set_abort(int on)
{
    s_abort = on;
}

void serial_out_purge(void)
{
    uart_ll_txfifo_rst(UART_LL_GET_HW(s_cfg.uart_num));
}

esp_err_t serial_out_flush(uint32_t timeout_ms)
{
    // In WATCH_PERIOD slices, so a cancel ends the wait: a job whose last
    // FIFO is stuck behind a paused printer has ripped everything already,
    // and nothing else would be checking.
    for (uint32_t waited = 0; ; waited += 100) {
        if (uart_wait_tx_done(s_cfg.uart_num, WATCH_PERIOD) == ESP_OK) return ESP_OK;
        if (s_abort) return ESP_FAIL;
        if (waited >= timeout_ms) return ESP_ERR_TIMEOUT;
    }
}

// The link's flow control: CTS on the ImageWriter's, none on a StyleWriter's (it may never
// raise HSKo). Whatever else turned it off or on for a moment puts this back.
static void apply_link_flow(void)
{
    uart_set_hw_flow_ctrl(s_cfg.uart_num,
                          s_link_cts ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE, 0);
}

// Round up: a gap of a few milliseconds must not become a poll of none.
static TickType_t ms_to_ticks_up(uint32_t ms)
{
    return ms ? (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS) : 0;
}

void serial_out_lend(int lend)
{
    if (lend) {
        uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(200));
        return;
    }
    // The same routing serial_out_init set up, CTS included: uart_set_pin
    // releases every pin it had before routing the new ones, so leaving CTS
    // out would tie the flow control to "clear" and overrun the printer.
    // Line inversion lives in the UART, so it survived the loan.
    uart_set_pin(s_cfg.uart_num, s_cfg.tx_pin, s_cfg.rx_pin, UART_PIN_NO_CHANGE,
                 (s_cfg.cts_pin >= 0 && !s_cfg.loopback) ? s_cfg.cts_pin : UART_PIN_NO_CHANGE);
    if (s_cfg.cts_pin >= 0 && !s_cfg.loopback) apply_link_flow();
    uart_flush_input(s_cfg.uart_num);
}

int serial_out_clear(void)
{
    // With no handshake line wired there is nothing to read, and "no idea"
    // must not be reported as "a printer is there". clear_to_send() assumes
    // clear in that case so transmission is not blocked; this does not.
    if (s_cfg.cts_pin < 0) return 0;
    return clear_to_send();
}

void serial_out_loopback_stats(size_t *bytes, uint32_t *crc32)
{
    if (bytes)  *bytes = s_lb_bytes;
    if (crc32) *crc32 = s_lb_crc;
}

int serial_out_loopback_wait(size_t expect, uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (s_lb_bytes >= expect) return 1;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_lb_bytes >= expect;
}

void serial_out_loopback_reset(void)
{
    s_lb_bytes = 0;
    s_lb_crc   = 0;
}

esp_err_t serial_out_send_unflowed(const uint8_t *buf, size_t n)
{
    // Not on a StyleWriter's link: this is the ImageWriter's CTRL-X, and only at its 9600.
    if (s_cfg.cts_pin < 0 || s_cfg.loopback || !s_link_cts) return ESP_ERR_INVALID_STATE;
    uart_set_hw_flow_ctrl(s_cfg.uart_num, UART_HW_FLOWCTRL_DISABLE, 0);
    int w = uart_tx_chars(s_cfg.uart_num, (const char *)buf, n);
    uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(100));
    apply_link_flow();
    return w == (int)n ? ESP_OK : ESP_FAIL;
}

void serial_out_rx_drop(void)
{
    uart_flush_input(s_cfg.uart_num);
}

int serial_out_query(const uint8_t *cmd, size_t n, uint8_t *reply, size_t cap,
                     uint32_t wait_ms)
{
    // -1 too on a StyleWriter's link (serial_out_set_link): flow controlled, a byte queued
    // with HSKo low would go out later at whatever baud the port has then.
    if (s_cfg.cts_pin < 0 || s_cfg.loopback || !s_link_cts) return -1;
    if (uart_tx_chars(s_cfg.uart_num, (const char *)cmd, n) != (int)n) return -1;
    int got = uart_read_bytes(s_cfg.uart_num, reply, 1, pdMS_TO_TICKS(wait_ms));
    if (got <= 0) return 0;
    while ((size_t)got < cap) {
        int more = uart_read_bytes(s_cfg.uart_num, reply + got, cap - (size_t)got,
                                   pdMS_TO_TICKS(50));
        if (more <= 0) break;
        got += more;
    }
    return got;
}

void serial_out_set_link(int baud, int cts)
{
    if (s_cfg.loopback) return;
    cts = cts && s_cfg.cts_pin >= 0;
    if (baud == s_link_baud && cts == s_link_cts) {
        uart_flush_input(s_cfg.uart_num);
        return;
    }
    // Bytes still in the FIFO were framed for the old link. Waiting a CTS-held byte out can
    // take forever, so past the wait it is thrown away: it must not go out later at the
    // wrong baud, or to a printer that never asked for it.
    if (uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(LINK_DRAIN_MS)) != ESP_OK) {
        serial_out_purge();
        ESP_LOGW(TAG, "link change: dropped what the transmitter still held");
    }
    uart_set_baudrate(s_cfg.uart_num, (uint32_t)baud);
    s_link_baud = baud;
    s_link_cts  = cts;
    apply_link_flow();
    uart_flush_input(s_cfg.uart_num);
    ESP_LOGD(TAG, "link: %d baud, %s", baud, cts ? "CTS" : "no flow control");   // flips every 60 s for an off ImageWriter
}

int serial_out_read(uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    if (s_cfg.loopback) return -1;
    if (!b || !cap) return 0;
    int got = uart_read_bytes(s_cfg.uart_num, b, 1, ms_to_ticks_up(first_ms));
    if (got <= 0) return 0;
    // One byte at a time while waiting, so the gap runs from the last byte (a read of the
    // whole remainder would wait out its full timeout after it); whatever is already
    // buffered comes in a piece.
    const TickType_t gap = ms_to_ticks_up(gap_ms);
    while ((size_t)got < cap) {
        int n = uart_read_bytes(s_cfg.uart_num, b + got, 1, gap);
        if (n <= 0) break;
        got += n;
        if ((size_t)got < cap) {
            int more = uart_read_bytes(s_cfg.uart_num, b + got, cap - (size_t)got, 0);
            if (more > 0) got += more;
        }
    }
    return got;
}

int serial_out_write_raw(const uint8_t *b, size_t n, uint32_t timeout_ms)
{
    if (s_cfg.loopback) return -1;
    const TickType_t limit = ms_to_ticks_up(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    while (n) {
        int w = uart_tx_chars(s_cfg.uart_num, (const char *)b, n);
        if (w < 0) return -1;
        b += w;
        n -= (size_t)w;
        if (!n) break;
        if ((TickType_t)(xTaskGetTickCount() - start) >= limit) return -1;
        // One tick, not serial_out_write's 100 ms: at 57600 the 128-byte FIFO empties in
        // 22 ms, and a longer sleep would starve the line.
        vTaskDelay(1);
    }
    return 0;
}

int serial_out_clock(int *hz)
{
    if (hz) *hz = s_clock_hz;
    return s_clocked;
}

int serial_out_clock_duty(void)
{
    if (s_cfg.cts_pin < 0) return -1;
    uint32_t lfsr = 0xACE1u, highs = 0;
    const uint32_t n = 200000;
    for (uint32_t i = 0; i < n; i++) {
        highs += gpio_ll_get_level(&GPIO, s_cfg.cts_pin);
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        for (volatile uint32_t k = lfsr & 31; k; k--) { }
    }
    return (int)(highs * 1000 / n);
}

int serial_out_break_take(void)
{
    if (!s_events) return 0;
    uart_event_t ev;
    int seen = 0;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE)
        if (ev.type == UART_BREAK) seen = 1;
    return seen;
}
