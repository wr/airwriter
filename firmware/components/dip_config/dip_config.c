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

    // DIP2-5 sit on IO39-42 -- MTCK/MTDO/MTDI/MTMS, still routed to the JTAG
    // IO-MUX function at reset -- and the DIP commons go straight to GND with
    // no series resistor. gpio_set_direction()/gpio_set_pull_mode() never call
    // the IO-MUX func-sel, so on those pins the JTAG function (MTDO is a
    // driven output) would stay live under a switch that shorts it to ground.
    // gpio_config() does select GPIO function; one call for every pin here
    // keeps that intact. Do not "simplify" this back to per-pin calls.
    //
    // Pins are gathered into an array and shifted through a runtime index
    // rather than shifted by s_btn_pin directly: on boards where every DIP
    // and button pin is compile-time -1 (no bank on this target), GCC folds
    // a bare `1ULL << s_btn_pin` to a provably-negative constant shift and
    // warns even though the `>= 0` guard means it never executes.
    int all_pin[9];
    for (int i = 0; i < 8; i++) all_pin[i] = s_dip_pin[i];
    all_pin[8] = s_btn_pin;

    uint64_t mask = 0;
    for (int i = 0; i < 9; i++)
        if (all_pin[i] >= 0) mask |= 1ULL << all_pin[i];

    if (!mask) return ESP_OK;

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    return xTaskCreate(dip_task, "dip_config", DIP_STACK, NULL, DIP_PRIO, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}
