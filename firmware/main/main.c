// SPDX-License-Identifier: GPL-3.0-or-later
//
// AirWriter application entry point. This is a bring-up stub: it runs the
// raster path once against a synthetic gradient so a target build proves the
// pipeline components link and execute on the S3, then idles. The real app
// (net_provision, ipp_server, serial_out, printer_state) replaces it.
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "halftone.h"
#include "imagewriter.h"

static const char *TAG = "airwriter";

#define SELFTEST_W 256
#define SELFTEST_H 16

static void count_out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)buf;
    *(size_t *)ctx += n;
}

// Halftones a horizontal gradient and hands it to the ImageWriter stage.
// Nothing is transmitted -- the output sink only counts bytes. It confirms
// the components run on target, not that the printer is happy.
static void raster_selftest(void)
{
    ht_t h;
    if (ht_init(&h, HT_ATKINSON, SELFTEST_W) != 0) {
        ESP_LOGE(TAG, "raster selftest: ht_init failed");
        return;
    }

    static iw_t w;
    size_t emitted = 0;
    iw_init(&w, SELFTEST_W, count_out, &emitted);
    iw_begin_page(&w);

    uint8_t gray[SELFTEST_W];
    uint8_t bits[(SELFTEST_W + 7) / 8];
    for (uint32_t y = 0; y < SELFTEST_H; y++) {
        for (uint32_t x = 0; x < SELFTEST_W; x++)
            gray[x] = (uint8_t)(x * 255 / (SELFTEST_W - 1));
        ht_row(&h, gray, bits);
        iw_row(&w, bits);
    }

    iw_end_page(&w);
    ht_free(&h);

    ESP_LOGI(TAG, "raster selftest: %ux%u page -> %u printer bytes",
             SELFTEST_W, SELFTEST_H, (unsigned)emitted);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AirWriter starting, %u bytes free", (unsigned)esp_get_free_heap_size());
    raster_selftest();
    ESP_LOGI(TAG, "bring-up stub: no network, no printer port yet");

    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
