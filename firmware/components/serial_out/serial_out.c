// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_out.h"
#include "serial_writer.h"
#include "serial_clock.h"

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

// W-1179: rev 2026.11 has a printer line on each socket, J2's on UART1 and J4's on UART0.
// One job at a time across the board, so every call below works on the selected port
// (serial_out_select, under the job lock) and only the line task watches them all.
typedef struct {
    serial_out_cfg_t cfg;
    int present;                     // serial_out_init_port ran
    volatile int parked;             // the socket does LocalTalk: pins let go, line unreported
    unsigned stall_limit;            // poll periods; see serial_writer.h
    pcnt_unit_handle_t pcnt;         // rising edges on cts_pin; NULL with no CTS line
    volatile int clocked, clock_hz;
    QueueHandle_t events;            // the UART driver's, read only by serial_out_break_take
    // The port's link right now (serial_out_set_link): the ImageWriter's, 9600 with CTS,
    // until a StyleWriter's is asked for. Read and written under the job lock, like the calls
    // that use it.
    int link_baud, link_cts;
    int last;                        // the line task's last level reported, -1 none
    serial_clock_t clk;
} port_t;

static port_t s_port[SERIAL_OUT_PORTS];
static int s_sel;                    // the selected port
static volatile int s_abort;         // see serial_out_set_abort()
static volatile size_t   s_lb_bytes;
static volatile uint32_t s_lb_crc;
static int s_line_task;              // the line task runs

#define P   (&s_port[s_sel])
#define CFG (P->cfg)
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
static int clear_to_send(const port_t *p)
{
    if (p->cfg.cts_pin < 0) return 1;
    int level = gpio_get_level(p->cfg.cts_pin);
    return p->cfg.invert_cts ? level == 1 : level == 0;
}

// The edge counter on cts_pin. Its channel goes through the GPIO matrix, so the pad stays the
// UART's CTS input.
static void clock_counter_init(port_t *p)
{
    pcnt_unit_config_t uc = { .low_limit = -1, .high_limit = CLOCK_LIMIT, .flags.accum_count = 1 };
    pcnt_chan_config_t cc = { .edge_gpio_num = p->cfg.cts_pin, .level_gpio_num = -1 };
    pcnt_channel_handle_t ch = NULL;
    if (pcnt_new_unit(&uc, &p->pcnt) != ESP_OK) { p->pcnt = NULL; return; }
    if (pcnt_new_channel(p->pcnt, &cc, &ch) != ESP_OK ||
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
        pcnt_unit_add_watch_point(p->pcnt, CLOCK_LIMIT) != ESP_OK ||
        pcnt_unit_enable(p->pcnt) != ESP_OK || pcnt_unit_start(p->pcnt) != ESP_OK) {
        ESP_LOGW(TAG, "no edge counter on the printer's handshake line");
        if (ch) pcnt_del_channel(ch);
        pcnt_del_unit(p->pcnt);
        p->pcnt = NULL;
    }
}

// One window on every watched port at once: each counter cleared, one 50 ms wait, each read.
// hz[i] is the rate in Hz, -1 with no counter. Returns whether any port has one.
static int clock_windows(int hz[SERIAL_OUT_PORTS])
{
    int any = 0;
    for (int i = 0; i < SERIAL_OUT_PORTS; i++) {
        hz[i] = -1;
        if (s_port[i].present && s_port[i].pcnt) {
            pcnt_unit_clear_count(s_port[i].pcnt);
            any = 1;
        }
    }
    if (!any) return 0;
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(CLOCK_WINDOW);
    int64_t dt = esp_timer_get_time() - t0;
    for (int i = 0; i < SERIAL_OUT_PORTS; i++) {
        if (!s_port[i].present || !s_port[i].pcnt) continue;
        int n = 0;
        pcnt_unit_get_count(s_port[i].pcnt, &n);
        hz[i] = dt > 0 && n > 0 ? (int)((int64_t)n * 1000000 / dt) : 0;
    }
    return 1;
}

static void line_port(port_t *p, int hz)
{
    int fast = hz >= CLOCK_HZ_MIN;
    if (p->parked) {
        // W-1179: a socket doing LocalTalk is still watched for a printer's clock
        // (serial_out_clock_of), but nothing is reported; the level is reported afresh when
        // it comes back.
        if (serial_clock_window(&p->clk, fast)) p->clocked = p->clk.clocked;
        p->clock_hz = hz;
        p->last = -1;
        return;
    }
    int paused = p->clk.clocked && fast ? p->clk.runs : 0;   // slow windows just ridden out
    if (serial_clock_window(&p->clk, fast)) {
        p->clocked = p->clk.clocked;
        p->clock_hz = hz;
        if (!fast) p->last = -1;                    // the level is reported afresh
        if (p->cfg.on_clock) p->cfg.on_clock(p->cfg.on_line_ctx, fast, hz);
    } else if (fast && p->clocked) {
        p->clock_hz = hz;
    }
    if (paused >= 2)
        ESP_LOGW(TAG, "the printer's clock stopped for about %d ms and came back", paused * 100);
    if (!p->clocked && !fast) {
        int now = clear_to_send(p);
        if (now != p->last) {
            p->last = now;
            ESP_LOGD(TAG, "printer %s", now ? "ready" : "not ready");
            if (p->cfg.on_line) p->cfg.on_line(p->cfg.on_line_ctx, now);
        }
    }
}

// Reports HSKo edges. Mid-job the line toggles on every printed line, so the
// callback must stay cheap; printer_state does the judging on its own clock.
// W-1102: each 100 ms also counts the line's edges for 50 ms. A clock (a QuickDraw
// LaserWriter switched on) is reported by on_clock, and while it runs the level is not.
// serial_clock.h decides: found in two windows, gone after 2 s. W-1179: every port with a
// handshake line, in the same windows.
static void line_task(void *arg)
{
    (void)arg;
    for (;;) {
        int hz[SERIAL_OUT_PORTS];
        int counted = clock_windows(hz);
        for (int i = 0; i < SERIAL_OUT_PORTS; i++)
            if (s_port[i].present && s_port[i].cfg.cts_pin >= 0 && !s_port[i].cfg.loopback)
                line_port(&s_port[i], hz[i]);
        vTaskDelay(counted ? WATCH_PERIOD - CLOCK_WINDOW : WATCH_PERIOD);
    }
}

// Drains whatever loopback returns so the RX ring never fills and stalls the
// peripheral. Only runs when loopback is on.
static void loopback_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        int n = uart_read_bytes(s_port[0].cfg.uart_num, buf, sizeof buf, pdMS_TO_TICKS(200));
        if (n > 0) {
            s_lb_crc = crc32_up(s_lb_crc, buf, (size_t)n);
            s_lb_bytes += (size_t)n;
        }
    }
}

esp_err_t serial_out_init(const serial_out_cfg_t *cfg)
{
    return serial_out_init_port(0, cfg);
}

// The port's UART as the printer line has it: the driver, and the speed and flow control
// given. Its pins and inversion are the caller's to set after.
static esp_err_t uart_up(port_t *p, int baud, int cts)
{
    uart_config_t uc = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        // CTS only. The printer never asks us to stop receiving, and RTS is
        // not wired to anything.
        .flow_ctrl = cts ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    // The event queue is for the printer's BREAK alone (W-1102, serial_out_break_take).
    esp_err_t err = uart_driver_install(p->cfg.uart_num, RX_RING, TX_RING, EVENT_QUEUE,
                                        &p->events, 0);
    if (err != ESP_OK) {
        p->events = NULL;
        return err;
    }
    return uart_param_config(p->cfg.uart_num, &uc);
}

static uint32_t line_inverse(const port_t *p)
{
    uint32_t inv = 0;
    if (p->cfg.invert_tx) inv |= UART_SIGNAL_TXD_INV;
    if (p->cfg.invert_rx) inv |= UART_SIGNAL_RXD_INV;
    if (p->cfg.invert_cts && p->cfg.cts_pin >= 0 && !p->cfg.loopback)
        inv |= UART_SIGNAL_CTS_INV;
    return inv;
}

esp_err_t serial_out_init_port(int port, const serial_out_cfg_t *cfg)
{
    if (port < 0 || port >= SERIAL_OUT_PORTS || s_port[port].present) return ESP_ERR_INVALID_ARG;
    // Loopback is port 0's alone: its reader task and counters are single.
    if (port && cfg->loopback) return ESP_ERR_NOT_SUPPORTED;
    port_t *p = &s_port[port];
    p->cfg = *cfg;
    if (!p->cfg.baud) p->cfg.baud = 9600;
    p->last = -1;
    p->stall_limit = (unsigned)(p->cfg.stall_abort_s > 0 ? p->cfg.stall_abort_s : 600)
                     * (1000u / 100u);   // WATCH_PERIOD is 100 ms

    esp_err_t err = ESP_OK;
    uint32_t inv = line_inverse(p);
    if (p->cfg.start_parked && !p->cfg.loopback) {
        // W-1186: the socket does LocalTalk, and its UART is its node's: no driver until the
        // port is unparked.
        p->parked = 1;
    } else {
        err = uart_up(p, p->cfg.baud, p->cfg.cts_pin >= 0 && !p->cfg.loopback);
        if (err != ESP_OK) return err;
        err = uart_set_pin(p->cfg.uart_num, p->cfg.tx_pin, p->cfg.rx_pin,
                           UART_PIN_NO_CHANGE,
                           (p->cfg.cts_pin >= 0 && !p->cfg.loopback)
                             ? p->cfg.cts_pin : UART_PIN_NO_CHANGE);
        if (err != ESP_OK) return err;
        if (inv) {
            err = uart_set_line_inverse(p->cfg.uart_num, inv);
            if (err != ESP_OK) return err;
        }
    }
    if (inv)
        ESP_LOGI(TAG, "line inversion: tx %s, rx %s, cts %s",
                 p->cfg.invert_tx ? "inverted" : "normal",
                 p->cfg.invert_rx ? "inverted" : "normal",
                 p->cfg.invert_cts ? "inverted" : "normal");

    if (p->cfg.loopback) {
        err = uart_set_loop_back(p->cfg.uart_num, true);
        if (err != ESP_OK) return err;
        xTaskCreate(loopback_task, "ser_lb", 3072, NULL, 6, NULL);
        ESP_LOGW(TAG, "loopback on: TX feeds RX inside the peripheral, nothing "
                      "reaches a printer");
    } else if (p->cfg.cts_pin >= 0) {
        clock_counter_init(p);
    }
    // No line to watch: say so once, so the state is not left unknown.
    if (p->cfg.loopback && p->cfg.on_line)         p->cfg.on_line(p->cfg.on_line_ctx, 1);
    else if (p->cfg.cts_pin < 0 && p->cfg.on_line) p->cfg.on_line(p->cfg.on_line_ctx, 0);

    p->link_baud = p->cfg.baud;
    p->link_cts  = p->cfg.cts_pin >= 0 && !p->cfg.loopback;
    p->present = 1;
    if (!p->cfg.loopback && p->cfg.cts_pin >= 0 && !s_line_task) {
        s_line_task = 1;
        xTaskCreate(line_task, "ser_line", 3072, NULL, 3, NULL);
    }

    ESP_LOGI(TAG, "port %d: uart%d at %d baud, tx %d rx %d cts %d", port + 1,
             p->cfg.uart_num, p->cfg.baud, p->cfg.tx_pin, p->cfg.rx_pin, p->cfg.cts_pin);
    return ESP_OK;
}

void serial_out_select(int port)
{
    if (port >= 0 && port < SERIAL_OUT_PORTS && s_port[port].present) s_sel = port;
}

int serial_out_selected(void) { return s_sel; }

int serial_out_present(int port)
{
    return port >= 0 && port < SERIAL_OUT_PORTS && s_port[port].present;
}

void serial_out_park(int port, int park)
{
    if (!serial_out_present(port)) return;
    port_t *p = &s_port[port];
    park = park ? 1 : 0;
    if (park == p->parked || p->cfg.loopback) return;
    if (park) {
        // What the transmitter holds goes first (a CTS-held byte is thrown away), then the
        // pad is a plain input with no pull: the socket's TashTalk owns the line now.
        if (uart_wait_tx_done(p->cfg.uart_num, pdMS_TO_TICKS(LINK_DRAIN_MS)) != ESP_OK)
            uart_ll_txfifo_rst(UART_LL_GET_HW(p->cfg.uart_num));
        p->parked = 1;
        // W-1186: the UART goes with the socket: its LocalTalk node installs its own driver.
        uart_driver_delete(p->cfg.uart_num);
        p->events = NULL;
        gpio_reset_pin(p->cfg.tx_pin);
        gpio_set_pull_mode(p->cfg.tx_pin, GPIO_FLOATING);
        gpio_set_direction(p->cfg.tx_pin, GPIO_MODE_INPUT);
        ESP_LOGI(TAG, "port %d: line let go", port + 1);
        return;
    }
    // W-1186: the printer line's driver again, on a UART its LocalTalk node has let go of.
    esp_err_t err = uart_up(p, p->link_baud, p->link_cts);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "port %d: its UART is not free: %s", port + 1, esp_err_to_name(err));
        return;
    }
    uart_set_line_inverse(p->cfg.uart_num, line_inverse(p));
    uart_set_pin(p->cfg.uart_num, p->cfg.tx_pin, p->cfg.rx_pin, UART_PIN_NO_CHANGE,
                 p->cfg.cts_pin >= 0 ? p->cfg.cts_pin : UART_PIN_NO_CHANGE);
    if (p->cfg.cts_pin >= 0)
        uart_set_hw_flow_ctrl(p->cfg.uart_num,
                              p->link_cts ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE, 0);
    uart_flush_input(p->cfg.uart_num);
    p->last = -1;
    // A clock found while parked is reported afresh, as at start.
    p->clocked = 0;
    p->clk = (serial_clock_t){ 0, 0 };
    p->parked = 0;
    ESP_LOGI(TAG, "port %d: line taken", port + 1);
}

int serial_out_parked(int port)
{
    return serial_out_present(port) && s_port[port].parked;
}

int serial_out_clear_of(int port)
{
    if (!serial_out_present(port) || s_port[port].cfg.cts_pin < 0) return 0;
    return clear_to_send(&s_port[port]);
}

int serial_out_junk_of(int port)
{
    if (!serial_out_present(port) || !s_port[port].events) return 0;
    uart_event_t ev;
    int n = 0;
    while (xQueueReceive(s_port[port].events, &ev, 0) == pdTRUE)
        n += ev.type == UART_FRAME_ERR || ev.type == UART_BREAK || ev.type == UART_PARITY_ERR;
    return n;
}

int serial_out_clock_of(int port, int *hz)
{
    if (!serial_out_present(port)) {
        if (hz) *hz = 0;
        return 0;
    }
    if (hz) *hz = s_port[port].clock_hz;
    return s_port[port].clocked;
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
    return uart_tx_chars(CFG.uart_num, (const char *)buf, n);
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
        .stall_limit = P->stall_limit,
    };
    if (P->parked) return ESP_FAIL;          // W-1186: its UART is LocalTalk's
    return serial_writer_write(&ops, buf, n) == 0 ? ESP_OK : ESP_FAIL;
}

void serial_out_set_abort(int on)
{
    s_abort = on;
}

void serial_out_purge(void)
{
    if (P->parked) return;
    uart_ll_txfifo_rst(UART_LL_GET_HW(CFG.uart_num));
}

esp_err_t serial_out_flush(uint32_t timeout_ms)
{
    // In WATCH_PERIOD slices, so a cancel ends the wait: a job whose last
    // FIFO is stuck behind a paused printer has ripped everything already,
    // and nothing else would be checking.
    if (P->parked) return ESP_FAIL;
    for (uint32_t waited = 0; ; waited += 100) {
        if (uart_wait_tx_done(CFG.uart_num, WATCH_PERIOD) == ESP_OK) return ESP_OK;
        if (s_abort) return ESP_FAIL;
        if (waited >= timeout_ms) return ESP_ERR_TIMEOUT;
    }
}

// The link's flow control: CTS on the ImageWriter's, none on a StyleWriter's (it may never
// raise HSKo). Whatever else turned it off or on for a moment puts this back.
static void apply_link_flow(void)
{
    uart_set_hw_flow_ctrl(CFG.uart_num,
                          P->link_cts ? UART_HW_FLOWCTRL_CTS : UART_HW_FLOWCTRL_DISABLE, 0);
}

// Round up: a gap of a few milliseconds must not become a poll of none.
static TickType_t ms_to_ticks_up(uint32_t ms)
{
    return ms ? (TickType_t)((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS) : 0;
}

void serial_out_lend(int lend)
{
    if (P->parked) return;
    if (lend) {
        uart_wait_tx_done(CFG.uart_num, pdMS_TO_TICKS(200));
        return;
    }
    // The same routing serial_out_init set up, CTS included: uart_set_pin
    // releases every pin it had before routing the new ones, so leaving CTS
    // out would tie the flow control to "clear" and overrun the printer.
    // Line inversion lives in the UART, so it survived the loan.
    uart_set_pin(CFG.uart_num, CFG.tx_pin, CFG.rx_pin, UART_PIN_NO_CHANGE,
                 (CFG.cts_pin >= 0 && !CFG.loopback) ? CFG.cts_pin : UART_PIN_NO_CHANGE);
    if (CFG.cts_pin >= 0 && !CFG.loopback) apply_link_flow();
    uart_flush_input(CFG.uart_num);
}

int serial_out_clear(void)
{
    // With no handshake line wired there is nothing to read, and "no idea"
    // must not be reported as "a printer is there". clear_to_send(P) assumes
    // clear in that case so transmission is not blocked; this does not.
    if (CFG.cts_pin < 0) return 0;
    return clear_to_send(P);
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
    if (CFG.cts_pin < 0 || CFG.loopback || !P->link_cts || P->parked) return ESP_ERR_INVALID_STATE;
    uart_set_hw_flow_ctrl(CFG.uart_num, UART_HW_FLOWCTRL_DISABLE, 0);
    int w = uart_tx_chars(CFG.uart_num, (const char *)buf, n);
    uart_wait_tx_done(CFG.uart_num, pdMS_TO_TICKS(100));
    apply_link_flow();
    return w == (int)n ? ESP_OK : ESP_FAIL;
}

void serial_out_rx_drop(void)
{
    if (P->parked) return;
    uart_flush_input(CFG.uart_num);
}

int serial_out_query(const uint8_t *cmd, size_t n, uint8_t *reply, size_t cap,
                     uint32_t wait_ms)
{
    // -1 too on a StyleWriter's link (serial_out_set_link): flow controlled, a byte queued
    // with HSKo low would go out later at whatever baud the port has then.
    if (CFG.cts_pin < 0 || CFG.loopback || !P->link_cts || P->parked) return -1;
    if (uart_tx_chars(CFG.uart_num, (const char *)cmd, n) != (int)n) return -1;
    int got = uart_read_bytes(CFG.uart_num, reply, 1, pdMS_TO_TICKS(wait_ms));
    if (got <= 0) return 0;
    while ((size_t)got < cap) {
        int more = uart_read_bytes(CFG.uart_num, reply + got, cap - (size_t)got,
                                   pdMS_TO_TICKS(50));
        if (more <= 0) break;
        got += more;
    }
    return got;
}

void serial_out_set_link(int baud, int cts)
{
    if (CFG.loopback || P->parked) return;
    cts = cts && CFG.cts_pin >= 0;
    if (baud == P->link_baud && cts == P->link_cts) {
        uart_flush_input(CFG.uart_num);
        return;
    }
    // Bytes still in the FIFO were framed for the old link. Waiting a CTS-held byte out can
    // take forever, so past the wait it is thrown away: it must not go out later at the
    // wrong baud, or to a printer that never asked for it.
    if (uart_wait_tx_done(CFG.uart_num, pdMS_TO_TICKS(LINK_DRAIN_MS)) != ESP_OK) {
        serial_out_purge();
        ESP_LOGW(TAG, "link change: dropped what the transmitter still held");
    }
    uart_set_baudrate(CFG.uart_num, (uint32_t)baud);
    P->link_baud = baud;
    P->link_cts  = cts;
    apply_link_flow();
    uart_flush_input(CFG.uart_num);
    ESP_LOGD(TAG, "link: %d baud, %s", baud, cts ? "CTS" : "no flow control");   // flips every 60 s for an off ImageWriter
}

int serial_out_read(uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    if (CFG.loopback) return -1;
    if (!b || !cap || P->parked) return 0;
    int got = uart_read_bytes(CFG.uart_num, b, 1, ms_to_ticks_up(first_ms));
    if (got <= 0) return 0;
    // One byte at a time while waiting, so the gap runs from the last byte (a read of the
    // whole remainder would wait out its full timeout after it); whatever is already
    // buffered comes in a piece.
    const TickType_t gap = ms_to_ticks_up(gap_ms);
    while ((size_t)got < cap) {
        int n = uart_read_bytes(CFG.uart_num, b + got, 1, gap);
        if (n <= 0) break;
        got += n;
        if ((size_t)got < cap) {
            int more = uart_read_bytes(CFG.uart_num, b + got, cap - (size_t)got, 0);
            if (more > 0) got += more;
        }
    }
    return got;
}

int serial_out_write_raw(const uint8_t *b, size_t n, uint32_t timeout_ms)
{
    if (CFG.loopback || P->parked) return -1;
    const TickType_t limit = ms_to_ticks_up(timeout_ms);
    const TickType_t start = xTaskGetTickCount();
    while (n) {
        int w = uart_tx_chars(CFG.uart_num, (const char *)b, n);
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
    if (hz) *hz = P->clock_hz;
    return P->clocked;
}

int serial_out_clock_live(void)
{
    if (CFG.cts_pin < 0) return 0;
    int last = gpio_ll_get_level(&GPIO, CFG.cts_pin), edges = 0;
    for (int i = 0; i < 2048 && edges < 8; i++) {
        int v = gpio_ll_get_level(&GPIO, CFG.cts_pin);
        edges += v != last;
        last = v;
    }
    return edges >= 8;
}

int serial_out_clock_duty(void)
{
    if (CFG.cts_pin < 0) return -1;
    uint32_t lfsr = 0xACE1u, highs = 0;
    const uint32_t n = 200000;
    for (uint32_t i = 0; i < n; i++) {
        highs += gpio_ll_get_level(&GPIO, CFG.cts_pin);
        lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
        for (volatile uint32_t k = lfsr & 31; k; k--) { }
    }
    return (int)(highs * 1000 / n);
}

int serial_out_break_take(void)
{
    if (!P->events) return 0;
    uart_event_t ev;
    int seen = 0;
    while (xQueueReceive(P->events, &ev, 0) == pdTRUE)
        if (ev.type == UART_BREAK) seen = 1;
    return seen;
}
