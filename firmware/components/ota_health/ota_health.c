// SPDX-License-Identifier: GPL-3.0-or-later
// The rollback health gate on the device: a 500 ms task that runs the pure
// gate (ota_health_core.c) while a new image is on trial, then acts once and
// ends. A crash or a reboot before the gate decides needs nothing from here:
// the bootloader finds the image still on trial and rolls it back itself.
//
// And the known-good record (W-1059, known_good_core.c): read at start, moved
// to the running slot once that image is kept and has printed, and consulted
// for where an update may go.
#include "ota_health.h"

#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "known_good_core.h"

static const char *TAG = "ota_health";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static oh_gate_t s_gate;
static volatile int s_trial;
// Kept: this image was not on trial at boot, or the gate's mark-valid took.
// Not the same as !s_trial: a gate that never started, or a mark-valid that
// failed, leaves the image to be rolled back at the next reboot.
static volatile int s_kept;
static const char *s_slot = "?";
static char s_rolled_back[32];

// The known-good record. s_kg guards all of it; promotion takes it across its
// flash write.
static SemaphoreHandle_t s_kg;
static const esp_partition_t *s_rec;    // the known_good partition; NULL on an old table
static int s_nslots, s_running = -1, s_printed;
static int s_good = -1;     // the slot the record names (ota_0 when blank); updates avoid it
static int s_named;         // the record names this running image
static char s_good_slot[8], s_good_ver[32];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int slot_of(const esp_partition_t *p)
{
    if (!p || p->type != ESP_PARTITION_TYPE_APP || p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
        return -1;
    return p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
}

static const esp_partition_t *slot_part(int slot)
{
    return slot < 0 ? NULL
        : esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                   ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot, NULL);
}

static int read_record(kg_rec_t two[2])
{
    return esp_partition_read(s_rec, 0, &two[0], sizeof two[0]) == ESP_OK &&
           esp_partition_read(s_rec, KG_SECTOR, &two[1], sizeof two[1]) == ESP_OK;
}

static const uint8_t *running_sha(void)
{
    return esp_app_get_description()->app_elf_sha256;
}

// The record was read or written: what the status page shows is what the
// power-up reset would trust, or nothing if the named slot holds another
// image now, or none (the reset then boots the newest kept one).
static void describe_good(const kg_rec_t two[2])
{
    const esp_partition_t *p = slot_part(s_good);
    esp_app_desc_t d;
    int ok = p && esp_ota_get_partition_description(p, &d) == ESP_OK;
    s_good_slot[0] = s_good_ver[0] = '\0';
    if (ok && kg_vouches(two, s_nslots, s_good, d.app_elf_sha256)) {
        strncpy(s_good_slot, p->label, sizeof s_good_slot - 1);
        strncpy(s_good_ver, d.version, sizeof s_good_ver - 1);
    }
}

static void kg_start(const esp_partition_t *run)
{
    s_kg = xSemaphoreCreateMutex();
    s_nslots = esp_ota_get_app_partition_count();
    s_running = slot_of(run);
    s_rec = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                     KG_PART_LABEL);
    if (!s_rec || s_rec->size < 2 * KG_SECTOR) {
        // An OTA cannot rewrite the partition table: this firmware OTA'd onto
        // a board still on the W-1046 table runs, but its power-up reset
        // still boots the factory slot (SPEC.md 3.11).
        s_rec = NULL;
        ESP_LOGE(TAG, "no known_good partition: this board needs one USB flash to get the "
                      "W-1059 partition table and bootloader");
        return;
    }
    kg_rec_t two[2];
    if (!read_record(two)) memset(two, 0xff, sizeof two);
    s_good = kg_slot(two, s_nslots);
    s_named = kg_names(two, s_nslots, s_running, running_sha());
    describe_good(two);
    if (s_good_slot[0])
        ESP_LOGI(TAG, "known-good firmware: %s %s%s", s_good_slot, s_good_ver,
                 kg_current(two) < 0 ? " (blank record: what a USB flash wrote)" : "");
    else
        ESP_LOGW(TAG, "ota_%d no longer holds the known-good image", s_good);
}

// Takes over the record if this image has earned it. Any task.
static void kg_promote(void)
{
    if (!s_rec || !s_kg) return;
    xSemaphoreTake(s_kg, portMAX_DELAY);
    if (kg_should_promote(s_kept, s_printed, s_running, s_named)) {
        kg_rec_t two[2], r, back;
        int copy = -1;
        esp_err_t err = read_record(two) ? ESP_OK : ESP_FAIL;
        if (err == ESP_OK) {
            // Never the current copy: cut short, this leaves the old record.
            copy = kg_next(two, s_running, running_sha(), &r);
            err = esp_partition_erase_range(s_rec, copy * KG_SECTOR, KG_SECTOR);
        }
        if (err == ESP_OK) err = esp_partition_write(s_rec, copy * KG_SECTOR, &r, sizeof r);
        if (err == ESP_OK) err = esp_partition_read(s_rec, copy * KG_SECTOR, &back, sizeof back);
        if (err == ESP_OK && memcmp(&back, &r, sizeof r)) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK) {
            two[copy] = r;
            s_good = s_running;
            s_named = 1;
            describe_good(two);
            ESP_LOGW(TAG, "%s %s is now the known-good firmware: the power-up reset boots it",
                     s_good_slot, s_good_ver);
        } else {
            // The next real print tries again.
            ESP_LOGE(TAG, "could not write the known-good record (%s)", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_kg);
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
            if (err != ESP_OK) vTaskDelete(NULL);   // still on trial, and the status says so
            s_kept = 1;
        } else {
            ESP_LOGE(TAG, "this firmware never became healthy (up 0x%x of 0x%x); rolling back",
                     (unsigned)up, (unsigned)need);
            esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
            // Returns only when there is no valid image to go back to. Then
            // this one is the best the board has: keep it rather than leave
            // it on trial for the next reboot to discard.
            ESP_LOGE(TAG, "no image to roll back to (%s); keeping this one", esp_err_to_name(err));
            s_kept = esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
        }
        s_trial = 0;
        kg_promote();       // a job may have printed during the trial
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
    esp_err_t se = run ? esp_ota_get_state_partition(run, &st) : ESP_FAIL;
    int trial = se == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    s_trial = trial;
    // No otadata entry for this slot: a USB flash, or a boot that fell past
    // otadata to the next good image. One that failed its trial and still
    // runs (nothing else would boot) is not kept.
    s_kept = se == ESP_ERR_NOT_FOUND ||
             (se == ESP_OK && (st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED));
    kg_start(run);
    if (!trial) {
        ESP_LOGI(TAG, "running %s", s_slot);    // an image already kept, or USB-flashed
        return;
    }
    oh_timing_t tm = OH_TIMING_DEFAULT;
    portENTER_CRITICAL(&s_mux);
    oh_init(&s_gate, &tm, has_creds, now_ms());
    portEXIT_CRITICAL(&s_mux);
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

void ota_health_printed(void)
{
    s_printed = 1;
    kg_promote();
}

const esp_partition_t *ota_health_update_target(void)
{
    if (!s_rec) return esp_ota_get_next_update_partition(NULL);
    xSemaphoreTake(s_kg, portMAX_DELAY);
    int t = kg_update_target(s_running, s_good, s_nslots);
    xSemaphoreGive(s_kg);
    return slot_part(t);
}

const char *ota_health_slot(void)        { return s_slot; }
int         ota_health_on_trial(void)    { return s_trial; }
const char *ota_health_rolled_back(void) { return s_rolled_back; }
const char *ota_health_known_good(void)  { return s_good_slot; }
const char *ota_health_known_good_version(void) { return s_good_ver; }
