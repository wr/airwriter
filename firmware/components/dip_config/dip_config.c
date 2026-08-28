// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the DIP bank and test button: owns the GPIOs and the 100ms
// poll task. Decode and the DIP debounce are the pure code in
// dip_config_core.c; the button gets its own, shorter debounce here since a
// deliberate press does not need three settled samples to be believed.
#include "dip_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DIP_POLL_MS   100
#define DIP_STACK     2560
#define DIP_PRIO      2

static const int s_dip_pin[8] = {
    CONFIG_AIRWRITER_DIP1_PIN, CONFIG_AIRWRITER_DIP2_PIN,
    CONFIG_AIRWRITER_DIP3_PIN, CONFIG_AIRWRITER_DIP4_PIN,
    CONFIG_AIRWRITER_DIP5_PIN, CONFIG_AIRWRITER_DIP6_PIN,
    CONFIG_AIRWRITER_DIP7_PIN, CONFIG_AIRWRITER_DIP8_PIN,
};
static const int s_btn_pin = CONFIG_AIRWRITER_TEST_BTN_PIN;

static void (*s_on_change)(const dip_config_t *cfg);
static void (*s_on_test_button)(void);

static void dip_task(void *arg)
{
    (void)arg;
    dip_deb_t dip_deb = {0};
    uint8_t   dip_out = 0xFF;   // impossible sample -- first settled read reports

    // Idle reads 1 (pulled up, not pressed); seeding the same value means a
    // press already held at boot is what it looks like -- a 1->0 edge on the
    // first settled sample -- rather than being swallowed.
    uint8_t btn_hist[2]  = {1, 1};
    uint8_t btn_stable   = 1;

    for (;;) {
        uint8_t on_bits = 0;
        for (int i = 0; i < 8; i++)
            if (s_dip_pin[i] >= 0 && gpio_get_level(s_dip_pin[i]) == 0)
                on_bits = (uint8_t)(on_bits | (1u << i));

        if (dip_stable(&dip_deb, on_bits, &dip_out) && s_on_change) {
            dip_config_t cfg;
            dip_decode(dip_out, &cfg);
            s_on_change(&cfg);
        }

        if (s_btn_pin >= 0) {
            btn_hist[0] = btn_hist[1];
            btn_hist[1] = (uint8_t)gpio_get_level(s_btn_pin);
            if (btn_hist[0] == btn_hist[1] && btn_hist[1] != btn_stable) {
                uint8_t was = btn_stable;
                btn_stable = btn_hist[1];
                if (was == 1 && btn_stable == 0 && s_on_test_button)
                    s_on_test_button();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(DIP_POLL_MS));
    }
}

esp_err_t dip_config_start(const dip_config_cb_t *cb)
{
    s_on_change      = cb ? cb->on_change      : NULL;
    s_on_test_button = cb ? cb->on_test_button : NULL;

    int any = s_btn_pin >= 0;
    for (int i = 0; i < 8; i++)
        if (s_dip_pin[i] >= 0) any = 1;
    if (!any) return ESP_OK;

    for (int i = 0; i < 8; i++) {
        if (s_dip_pin[i] < 0) continue;
        esp_err_t err = gpio_set_direction(s_dip_pin[i], GPIO_MODE_INPUT);
        if (err != ESP_OK) return err;
        err = gpio_set_pull_mode(s_dip_pin[i], GPIO_PULLUP_ONLY);
        if (err != ESP_OK) return err;
    }
    if (s_btn_pin >= 0) {
        esp_err_t err = gpio_set_direction(s_btn_pin, GPIO_MODE_INPUT);
        if (err != ESP_OK) return err;
        err = gpio_set_pull_mode(s_btn_pin, GPIO_PULLUP_ONLY);
        if (err != ESP_OK) return err;
    }

    return xTaskCreate(dip_task, "dip_config", DIP_STACK, NULL, DIP_PRIO, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
