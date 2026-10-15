// SPDX-License-Identifier: GPL-3.0-or-later
// The panel chain. Rendering is the pure core; this task owns time and the
// wire. On a devkit with no strip (pin -1) start() is a successful no-op, so
// main never has to care. The IO48 boot clamp hands the pin over to the RMT
// driver here -- by the first frame the line is driven, never floating.
#include "led_grid.h"
#include "led_grid_core.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "printer_state.h"

static led_strip_handle_t s_strip;
static int s_count;
static volatile uint8_t s_gamer;   // DIP7, set from dip_config's task

static void lg_task(void *arg)
{
    (void)arg;
    uint8_t frame[LG_NUM_LEDS][3];
    for (;;) {
        // Reads printer_state directly each tick rather than being pushed a
        // state, so two racing posters can never deliver an update to this
        // task out of order -- the next 30ms tick just re-reads whatever is
        // current.
        ps_view_t v = printer_state_view();
        lg_opts_t o = { .gamer = s_gamer };
        lg_render(&v, &o, (uint32_t)(esp_timer_get_time() / 1000), frame);
        for (int i = 0; i < s_count && i < LG_NUM_LEDS; i++)
            led_strip_set_pixel(s_strip, (uint32_t)i,
                                 frame[i][0], frame[i][1], frame[i][2]);
        led_strip_refresh(s_strip);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

esp_err_t led_grid_start(void)
{
    if (CONFIG_AIRWRITER_LED_DATA_PIN < 0) return ESP_OK;

    s_count = CONFIG_AIRWRITER_LED_COUNT;
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_AIRWRITER_LED_DATA_PIN,
        .max_leds = (uint32_t)s_count,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) return err;
    led_strip_clear(s_strip);
    return xTaskCreate(lg_task, "led_grid", 3072, NULL, 3, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void led_grid_set_gamer(int on)
{
    s_gamer = on ? 1 : 0;
}
