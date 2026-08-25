// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_out.h"

#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "serial";

#define TX_RING      2048
#define RX_RING      1024   // only used for the loopback self-check
#define WATCH_PERIOD pdMS_TO_TICKS(100)

static serial_out_cfg_t s_cfg;
static volatile serial_out_state_t s_state = SERIAL_OUT_READY;
static volatile size_t   s_lb_bytes;
static volatile uint32_t s_lb_crc;

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

// The printer stalls constantly and briefly while its head returns; only a
// stall that outlasts the timeout means paper. SPEC.md 3.9.
static void watchdog_task(void *arg)
{
    (void)arg;
    TickType_t blocked_since = 0;
    const TickType_t limit = pdMS_TO_TICKS(1000 * (s_cfg.stall_timeout_s ?: 30));

    for (;;) {
        vTaskDelay(WATCH_PERIOD);

        if (clear_to_send()) {
            if (s_state != SERIAL_OUT_READY) {
                ESP_LOGI(TAG, "printer ready again");
                s_state = SERIAL_OUT_READY;
                if (s_cfg.on_state) s_cfg.on_state(s_cfg.on_state_ctx, s_state);
            }
            blocked_since = 0;
            continue;
        }

        if (blocked_since == 0) {
            blocked_since = xTaskGetTickCount();
        } else if (s_state == SERIAL_OUT_READY &&
                   xTaskGetTickCount() - blocked_since > limit) {
            ESP_LOGW(TAG, "printer has held HSKo low for %ds: out of paper, jammed "
                          "or offline", s_cfg.stall_timeout_s ?: 30);
            s_state = SERIAL_OUT_BLOCKED;
            if (s_cfg.on_state) s_cfg.on_state(s_cfg.on_state_ctx, s_state);
        }
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

    esp_err_t err = uart_driver_install(s_cfg.uart_num, RX_RING, TX_RING, 0, NULL, 0);
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
        xTaskCreate(watchdog_task, "ser_wd", 3072, NULL, 3, NULL);
    }

    ESP_LOGI(TAG, "uart%d at %d baud, tx %d rx %d cts %d",
             s_cfg.uart_num, s_cfg.baud, s_cfg.tx_pin, s_cfg.rx_pin, s_cfg.cts_pin);
    return ESP_OK;
}

esp_err_t serial_out_write(const uint8_t *buf, size_t n)
{
    while (n) {
        int w = uart_write_bytes(s_cfg.uart_num, (const char *)buf, n);
        if (w < 0) return ESP_FAIL;
        buf += w;
        n   -= (size_t)w;
    }
    return ESP_OK;
}

esp_err_t serial_out_flush(uint32_t timeout_ms)
{
    return uart_wait_tx_done(s_cfg.uart_num, pdMS_TO_TICKS(timeout_ms));
}

serial_out_state_t serial_out_state(void) { return s_state; }

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
