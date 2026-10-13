// SPDX-License-Identifier: GPL-3.0-or-later
#include "pic_prog.h"

#include <stdlib.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "pic_prog";

extern const char tashtalk_hex_start[] asm("_binary_tashtalk_2_1_3_hex_start");
extern const char tashtalk_hex_end[]   asm("_binary_tashtalk_2_1_3_hex_end");

#define PIN_MCLR CONFIG_AIRWRITER_PIC_MCLR_PIN
#define PIN_CLK  CONFIG_AIRWRITER_PIC_CLK_PIN
#define PIN_DAT  CONFIG_AIRWRITER_PIC_DAT_PIN

int pic_prog_available(void)
{
    return PIN_MCLR >= 0 && PIN_CLK >= 0 && PIN_DAT >= 0;
}

#if CONFIG_AIRWRITER_PIC_MCLR_PIN >= 0 && CONFIG_AIRWRITER_PIC_CLK_PIN >= 0 && \
    CONFIG_AIRWRITER_PIC_DAT_PIN >= 0

static void h_mclr(void *ctx, int v) { gpio_set_level(PIN_MCLR, v); }
static void h_clk(void *ctx, int v)  { gpio_set_level(PIN_CLK, v); }

static void h_dat_out(void *ctx, int v)
{
    gpio_set_level(PIN_DAT, v);
    gpio_set_direction(PIN_DAT, GPIO_MODE_INPUT_OUTPUT);
}

static void h_dat_release(void *ctx) { gpio_set_direction(PIN_DAT, GPIO_MODE_INPUT); }
static int  h_dat_in(void *ctx)      { return gpio_get_level(PIN_DAT); }

// Microsecond waits spin; the 3-10 ms programming and erase waits sleep, so a
// two-second flash doesn't starve lower-priority tasks. ICSP has no maximum
// on any of these gaps, so oversleeping by a tick costs only time.
static void h_delay(void *ctx, uint32_t us)
{
    if (us < 1000) { esp_rom_delay_us(us); return; }
    TickType_t t = pdMS_TO_TICKS((us + 999) / 1000);
    vTaskDelay(t + 1);
}

static void pins_claim(void)
{
    // MCLR low first: the PIC sits in reset with its outputs tristated before
    // the ESP32 starts driving the lines its UART would otherwise use.
    gpio_set_level(PIN_MCLR, 0);
    gpio_set_direction(PIN_MCLR, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(PIN_MCLR, GPIO_FLOATING);
    gpio_set_level(PIN_CLK, 0);
    gpio_set_direction(PIN_CLK, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(PIN_CLK, GPIO_FLOATING);
    gpio_set_level(PIN_DAT, 0);
    gpio_set_direction(PIN_DAT, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_pull_mode(PIN_DAT, GPIO_FLOATING);
}

// Back to the boot state: inputs with pull-ups. MCLR rises and TashTalk runs.
static void pins_release(void)
{
    gpio_reset_pin(PIN_CLK);
    gpio_reset_pin(PIN_DAT);
    gpio_reset_pin(PIN_MCLR);
}

pic_err_t pic_prog_flash_tashtalk(pic_result_t *res)
{
    static SemaphoreHandle_t lock;
    static portMUX_TYPE init_mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&init_mux);
    if (!lock) lock = xSemaphoreCreateMutex();
    taskEXIT_CRITICAL(&init_mux);

    *res = (pic_result_t){0};
    if (!lock) return res->err = PIC_ERR_NOMEM;
    if (xSemaphoreTake(lock, 0) != pdTRUE) return res->err = PIC_ERR_BUSY;

    pic_image_t *img = malloc(sizeof *img);
    if (!img) { xSemaphoreGive(lock); return res->err = PIC_ERR_NOMEM; }

    int line = 0;
    pic_hex_err_t he = pic_hex_parse(tashtalk_hex_start,
                                     (size_t)(tashtalk_hex_end - tashtalk_hex_start), img, &line);
    if (he != PIC_HEX_OK) {
        // The image is compiled in and host-tested; this means a bad build.
        ESP_LOGE(TAG, "embedded hex: error %d at line %d", he, line);
        free(img);
        xSemaphoreGive(lock);
        return res->err = PIC_ERR_IMAGE;
    }
    pic_image_enable_lvp(img);

    pic_hal_t hal = { h_mclr, h_clk, h_dat_out, h_dat_release, h_dat_in, h_delay, NULL };
    ESP_LOGW(TAG, "flashing TashTalk 2.1.3 (MCLR=%d CLK=%d DAT=%d)", PIN_MCLR, PIN_CLK, PIN_DAT);
    pins_claim();
    pic_err_t e = pic_flash(&hal, img, res);
    pins_release();
    free(img);

    if (e == PIC_OK)
        ESP_LOGI(TAG, "flashed and verified, device ID 0x%04X", res->devid);
    else if (e == PIC_ERR_VERIFY)
        ESP_LOGE(TAG, "verify failed at 0x%04X: want 0x%04X got 0x%04X",
                 (unsigned)res->bad_addr, res->want, res->got);
    else
        ESP_LOGE(TAG, "%s (device ID read 0x%04X)", pic_err_str(e), res->devid);

    xSemaphoreGive(lock);
    return e;
}

#else

pic_err_t pic_prog_flash_tashtalk(pic_result_t *res)
{
    *res = (pic_result_t){ .err = PIC_ERR_UNAVAILABLE };
    return PIC_ERR_UNAVAILABLE;
}

#endif
