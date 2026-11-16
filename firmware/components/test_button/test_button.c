// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the test button: owns its GPIO and the 100ms poll task. The
// tap / hold / reset timing is the pure code in test_button_core.c.
#include "test_button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define BTN_POLL_MS   100
#define BTN_STACK     2560
#define BTN_PRIO      2

static void (*s_on_tap)(void);
static void (*s_on_hold)(btn_ev_t ev);

static void btn_task(void *arg)
{
    int pin = (int)(intptr_t)arg;
    // A press already down at the first sample is the tail of the
    // bootloader's factory-reset hold, and btn_step ignores it until release.
    btn_t btn = {0};

    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        btn_ev_t ev = btn_step(&btn, gpio_get_level(pin) == 0, now);
        if (ev == BTN_EV_TAP) {
            if (s_on_tap) s_on_tap();
        } else if (ev != BTN_EV_NONE && s_on_hold) {
            s_on_hold(ev);
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

esp_err_t test_button_start(const test_button_cb_t *cb)
{
    s_on_tap  = cb ? cb->on_tap  : NULL;
    s_on_hold = cb ? cb->on_hold : NULL;

    // Through a runtime variable, not the Kconfig value itself: where the pin
    // is compile-time -1, GCC folds `1ULL << pin` to a negative constant shift
    // and warns, even behind the `< 0` return.
    volatile int pin = CONFIG_AIRWRITER_TEST_BTN_PIN;
    if (pin < 0) return ESP_OK;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) return err;

    return xTaskCreate(btn_task, "test_btn", BTN_STACK, (void *)(intptr_t)pin, BTN_PRIO,
                       NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
