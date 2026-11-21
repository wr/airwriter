// SPDX-License-Identifier: GPL-3.0-or-later
#include "pic_prog.h"

#include <stdatomic.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pic_prog";

extern const char tashtalk_hex_start[] asm("_binary_tashtalk_2_1_3_hex_start");
extern const char tashtalk_hex_end[]   asm("_binary_tashtalk_2_1_3_hex_end");

pic_pins_t pic_prog_u7(void)
{
    return (pic_pins_t){ CONFIG_AIRWRITER_PIC_MCLR_PIN, CONFIG_AIRWRITER_PIC_CLK_PIN,
                         CONFIG_AIRWRITER_PIC_DAT_PIN, 0 };
}

static int pins_ok(const pic_pins_t *p) { return p->mclr >= 0 && p->clk >= 0 && p->dat >= 0; }

int pic_prog_available(void)
{
    pic_pins_t u7 = pic_prog_u7();
    return pins_ok(&u7);
}

static atomic_flag s_busy = ATOMIC_FLAG_INIT;     // a flash or an ID read, on any PIC

static void h_mclr(void *ctx, int v) { gpio_set_level(((const pic_pins_t *)ctx)->mclr, v); }
static void h_clk(void *ctx, int v)  { gpio_set_level(((const pic_pins_t *)ctx)->clk, v); }

static void h_dat_out(void *ctx, int v)
{
    int dat = ((const pic_pins_t *)ctx)->dat;
    gpio_set_level(dat, v);
    gpio_set_direction(dat, GPIO_MODE_INPUT_OUTPUT);
}

static void h_dat_release(void *ctx) { gpio_set_direction(((const pic_pins_t *)ctx)->dat, GPIO_MODE_INPUT); }
static int  h_dat_in(void *ctx)      { return gpio_get_level(((const pic_pins_t *)ctx)->dat); }

// Microsecond waits spin; the 3-10 ms programming and erase waits sleep, so a
// two-second flash doesn't starve lower-priority tasks. ICSP has no maximum
// on any of these gaps, so oversleeping by a tick costs only time.
static void h_delay(void *ctx, uint32_t us)
{
    (void)ctx;
    uint32_t t = pic_sleep_ticks(us, portTICK_PERIOD_MS * 1000u);
    if (t) vTaskDelay(t);
    else   esp_rom_delay_us(us);
}

// Starts from gpio_reset_pin so a pad some peripheral had routed through the
// GPIO matrix becomes a plain GPIO again. MCLR goes low before the others are
// driven: the PIC sits in reset, outputs tristated, before the ESP32 drives RA0.
// DAT gets a pull-down so an empty socket reads 0x0000 ("no PIC") rather than
// whatever a floating pin makes up; a real PIC drives through it easily.
static void pins_claim(const pic_pins_t *p)
{
    gpio_reset_pin(p->mclr);
    gpio_set_pull_mode(p->mclr, GPIO_FLOATING);
    gpio_set_level(p->mclr, 0);
    gpio_set_direction(p->mclr, GPIO_MODE_OUTPUT);

    gpio_reset_pin(p->clk);
    gpio_set_pull_mode(p->clk, GPIO_FLOATING);
    gpio_set_level(p->clk, 0);
    gpio_set_direction(p->clk, GPIO_MODE_OUTPUT);

    gpio_reset_pin(p->dat);
    gpio_set_pull_mode(p->dat, GPIO_PULLDOWN_ONLY);
    gpio_set_direction(p->dat, GPIO_MODE_INPUT);
}

// Back to inputs with pull-ups: MCLR rises and TashTalk runs; CLK idles high, which is its
// UART's idle level and what app_main's boot clamp drove. With keep_reset (W-1179, a socket
// doing the printer job), MCLR stays low and CLK and DAT are pulled down: nothing else drives
// them then, and a PIC in reset listens there for the LVP key.
static void pins_release(const pic_pins_t *p)
{
    if (p->keep_reset) {
        gpio_set_level(p->clk, 0);
        gpio_set_direction(p->clk, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p->clk, GPIO_PULLDOWN_ONLY);
        gpio_set_direction(p->dat, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p->dat, GPIO_PULLDOWN_ONLY);
        gpio_set_level(p->mclr, 0);
        return;
    }
    gpio_reset_pin(p->clk);
    gpio_reset_pin(p->dat);
    gpio_reset_pin(p->mclr);
}

static pic_hal_t hal_for(const pic_pins_t *p)
{
    return (pic_hal_t){ h_mclr, h_clk, h_dat_out, h_dat_release, h_dat_in, h_delay, (void *)p };
}

pic_err_t pic_prog_read_id_on(const pic_pins_t *pins, uint16_t *devid)
{
    *devid = 0;
    if (!pins_ok(pins)) return PIC_ERR_UNAVAILABLE;
    if (atomic_flag_test_and_set(&s_busy)) return PIC_ERR_BUSY;
    pins_claim(pins);
    pic_hal_t hal = hal_for(pins);
    pic_err_t e = pic_read_id(&hal, devid);
    pins_release(pins);
    atomic_flag_clear(&s_busy);
    return e;
}

pic_err_t pic_prog_flash_tashtalk_on(const pic_pins_t *pins, pic_result_t *res)
{
    *res = (pic_result_t){0};
    if (!pins_ok(pins)) return res->err = PIC_ERR_UNAVAILABLE;
    if (atomic_flag_test_and_set(&s_busy)) return res->err = PIC_ERR_BUSY;

    pic_image_t *img = malloc(sizeof *img);
    if (!img) { atomic_flag_clear(&s_busy); return res->err = PIC_ERR_NOMEM; }

    int line = 0;
    pic_hex_err_t he = pic_hex_parse(tashtalk_hex_start,
                                     (size_t)(tashtalk_hex_end - tashtalk_hex_start), img, &line);
    if (he != PIC_HEX_OK) {
        // The image is compiled in and host-tested; this means a bad build.
        ESP_LOGE(TAG, "embedded hex: error %d at line %d", he, line);
        free(img);
        atomic_flag_clear(&s_busy);
        return res->err = PIC_ERR_IMAGE;
    }
    pic_image_enable_lvp(img);

    ESP_LOGW(TAG, "flashing TashTalk 2.1.3 (MCLR=%d CLK=%d DAT=%d)", pins->mclr, pins->clk,
             pins->dat);
    pins_claim(pins);
    pic_hal_t hal = hal_for(pins);
    pic_err_t e = pic_flash(&hal, img, res);
    pins_release(pins);
    free(img);

    if (e == PIC_OK) {
        const uint16_t *c = res->cfg;
        ESP_LOGI(TAG, "flashed and verified, device ID 0x%04X", res->devid);
        ESP_LOGI(TAG, "config 8000..800A: %04X %04X %04X %04X | %04X %04X | %04X | "
                 "%04X %04X | %04X %04X", c[0], c[1], c[2], c[3], c[4], c[5], c[6],
                 c[7], c[8], c[9], c[10]);
    }
    else if (e == PIC_ERR_VERIFY)
        ESP_LOGE(TAG, "verify failed at 0x%04X: want 0x%04X got 0x%04X",
                 (unsigned)res->bad_addr, res->want, res->got);
    else
        ESP_LOGE(TAG, "%s (device ID read 0x%04X)", pic_err_str(e), res->devid);

    atomic_flag_clear(&s_busy);
    return e;
}

pic_err_t pic_prog_read_id(uint16_t *devid)
{
    pic_pins_t u7 = pic_prog_u7();
    return pic_prog_read_id_on(&u7, devid);
}

pic_err_t pic_prog_flash_tashtalk(pic_result_t *res)
{
    pic_pins_t u7 = pic_prog_u7();
    return pic_prog_flash_tashtalk_on(&u7, res);
}
