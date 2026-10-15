// SPDX-License-Identifier: GPL-3.0-or-later
//
// LocalTalk bring-up (W-1050): listen to TashTalk and log every frame it
// relays. Bench builds only (CONFIG_AIRWRITER_LT_SNIFF); transmits nothing.
//
// TashTalk relays every frame it hears, whatever its destination, at 1 Mbaud.
// A 0x00 byte escapes: 00 FF is a literal zero, 00 FD ends a frame cleanly,
// 00 FE is a framing error, 00 FA an abort, 00 FC a bad CRC (only reported
// when CRC checking is switched on, which this build leaves off). So the FCS
// is checked here: CRC-CCITT as in HDLC, which leaves 0xF0B8 over a good frame.
#include "lt_sniff.h"
#include "sdkconfig.h"

#if CONFIG_AIRWRITER_LT_SNIFF

#include <stdio.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lt";

#define LT_UART     UART_NUM_2
#define LT_BAUD     1000000
#define LT_TX_PIN   CONFIG_AIRWRITER_PIC_CLK_PIN    // LT_UART_TX, TashTalk's RX
#define LT_RX_PIN   CONFIG_AIRWRITER_PIC_DAT_PIN    // LT_UART_RX, TashTalk's TX
#define FRAME_MAX   640                             // LLAP's largest is 605

static uint16_t fcs16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

static const char *llap_type(uint8_t t)
{
    switch (t) {
    case 0x01: return "DDP short";
    case 0x02: return "DDP long";
    case 0x81: return "ENQ";
    case 0x82: return "ACK";
    case 0x84: return "RTS";
    case 0x85: return "CTS";
    default:   return "?";
    }
}

static void log_frame(const uint8_t *f, size_t n, const char *how)
{
    if (n < 3) { ESP_LOGW(TAG, "%s: runt, %u bytes", how, (unsigned)n); return; }
    // dst, src, LLAP type; data frames then carry the FCS in their last two.
    int good = n >= 5 && fcs16(f, n) == 0xF0B8;
    char hex[3 * 24 + 1] = {0};
    size_t show = n < 24 ? n : 24;
    for (size_t i = 0; i < show; i++) sprintf(hex + 3 * i, "%02x ", f[i]);
    ESP_LOGI(TAG, "%s %3u B  dst %3u src %3u  %-9s fcs %s  | %s%s", how, (unsigned)n,
             f[0], f[1], llap_type(f[2]), good ? "ok " : "BAD", hex, n > show ? "..." : "");
}

static void sniff_task(void *arg)
{
    (void)arg;
    static uint8_t frame[FRAME_MAX];
    uint8_t buf[256];
    size_t len = 0;
    int esc = 0;
    unsigned frames = 0, errors = 0;

    for (;;) {
        int n = uart_read_bytes(LT_UART, buf, sizeof buf, pdMS_TO_TICKS(1000));
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            if (!esc) {
                if (b == 0x00) { esc = 1; continue; }
                if (len < FRAME_MAX) frame[len++] = b;
                continue;
            }
            esc = 0;
            switch (b) {
            case 0xFF: if (len < FRAME_MAX) frame[len++] = 0x00; break;
            case 0xFD: frames++; log_frame(frame, len, "frame"); len = 0; break;
            case 0xFC: frames++; log_frame(frame, len, "crc!!"); len = 0; break;
            case 0xFE: errors++; ESP_LOGW(TAG, "framing error after %u bytes", (unsigned)len); len = 0; break;
            case 0xFA: errors++; ESP_LOGW(TAG, "aborted frame after %u bytes", (unsigned)len); len = 0; break;
            default:   errors++; ESP_LOGW(TAG, "unknown escape 00 %02x", b); len = 0; break;
            }
        }
        static unsigned quiet;
        if (n <= 0 && ++quiet % 30 == 0)
            ESP_LOGI(TAG, "listening: %u frames, %u errors so far", frames, errors);
    }
}

void lt_sniff_start(void)
{
    uart_config_t uc = {
        .baud_rate  = LT_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LT_UART, 8192, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LT_UART, &uc));
    // TX idles high, as app_main's clamp left it; nothing is ever sent.
    ESP_ERROR_CHECK(uart_set_pin(LT_UART, LT_TX_PIN, LT_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    xTaskCreate(sniff_task, "lt_sniff", 4096, NULL, 3, NULL);
    ESP_LOGW(TAG, "LocalTalk sniff on: UART2 %d baud, rx IO%d -- bench build, transmits nothing",
             LT_BAUD, LT_RX_PIN);
}

#else
void lt_sniff_start(void) {}
#endif
