// SPDX-License-Identifier: GPL-3.0-or-later
// The rollback health gate on the device: a 500 ms task that runs the pure
// gate (ota_health_core.c) while a new image is on trial, then acts once and
// ends. A crash or a reboot before the gate decides needs nothing from here:
// the bootloader finds the image still on trial and rolls it back itself.
#include "ota_health.h"

#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ota_health";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static oh_gate_t s_gate;
static volatile int s_trial;
static const char *s_slot = "?";
static char s_rolled_back[32];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void gate_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        portENTER_CRITICAL(&s_mux);
        oh_action_t a = oh_update(&s_gate, now_ms());
        uint32_t up = s_gate.up, need = s_gate.need;
        portEXIT_CRITICAL(&s_mux);
        if (a == OH_WAIT) continue;

        if (a == OH_MARK_VALID) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "this firmware is healthy; keeping it (%s)", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "this firmware never became healthy (up 0x%x of 0x%x); rolling back",
                     (unsigned)up, (unsigned)need);
            esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
            // Returns only when there is no valid image to go back to. Then
            // this one is the best the board has: keep it rather than leave
            // it on trial for the next reboot to discard.
            ESP_LOGE(TAG, "no image to roll back to (%s); keeping this one", esp_err_to_name(err));
            esp_ota_mark_app_valid_cancel_rollback();
        }
        s_trial = 0;
        vTaskDelete(NULL);
    }
}

void ota_health_start(int has_creds)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    s_slot = run ? run->label : "?";

    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (bad && esp_ota_get_partition_description(bad, &desc) == ESP_OK) {
        strncpy(s_rolled_back, desc.version, sizeof s_rolled_back - 1);
        ESP_LOGW(TAG, "%s (%s) was rolled back", bad->label, s_rolled_back);
    }

    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running %s", s_slot);    // factory, or an image already kept
        return;
    }
    oh_timing_t tm = OH_TIMING_DEFAULT;
    portENTER_CRITICAL(&s_mux);
    oh_init(&s_gate, &tm, has_creds, now_ms());
    portEXIT_CRITICAL(&s_mux);
    s_trial = 1;
    ESP_LOGW(TAG, "running %s on trial: kept once %s for %us, rolled back if not by %us",
             s_slot, has_creds ? "Wi-Fi, IPP and the web UI are up" : "the setup portal is up",
             (unsigned)(tm.settle_ms / 1000), (unsigned)(tm.deadline_ms / 1000));
    if (xTaskCreate(gate_task, "ota_health", 3072, NULL, 2, NULL) != pdPASS) {
        // No gate means no verdict, and the next reboot rolls this image
        // back. That is the safe side for an image that cannot start a task.
        ESP_LOGE(TAG, "could not start the gate");
        s_trial = 0;
    }
}

void ota_health_set(uint32_t bits, int on)
{
    if (!s_trial) return;
    portENTER_CRITICAL(&s_mux);
    oh_set(&s_gate, bits, on, now_ms());
    portEXIT_CRITICAL(&s_mux);
}

const char *ota_health_slot(void)        { return s_slot; }
int         ota_health_on_trial(void)    { return s_trial; }
const char *ota_health_rolled_back(void) { return s_rolled_back; }
