// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bootloader hooks (W-1046, W-1059). They run before the bootloader picks a
// slot, and they are the one part of the firmware an update cannot replace:
// a mistake here needs a USB flash on every board. Keep them small.
//
// The power-up reset holds the bootloader for up to 5 s while the test button
// is down, and until app_main runs nothing drives the pins that must not
// float (docs/traps.md): IO48 floating feeds the LED chain noise, which can
// brownout-loop the board on a 500 mA port. So the pins are clamped first, to
// the levels app_main's first statements set. The numbers are the S3 board's:
// the bootloader build does not see main's Kconfig, and main.c checks them.
#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "hal/wdt_hal.h"
#include "soc/gpio_sig_map.h"
#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "bootloader_flash_priv.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "known_good_core.h"

// The bootloader's link pulls this file in by this symbol; its hooks are weak.
void bootloader_hooks_include(void)
{
}

#if CONFIG_IDF_TARGET_ESP32S3
static const char *TAG = "airwriter";

#define RESET_PIN       14      // the test button, SW3 (AIRWRITER_TEST_BTN_PIN)
#define RESET_HOLD_S    5
#define MAX_SLOTS       16

static void drive(uint32_t pin, uint32_t level)
{
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_set_level(&GPIO, pin, level);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, pin);
}

typedef struct {
    esp_partition_pos_t ota[MAX_SLOTS];     // by OTA number
    int nslots;
    esp_partition_pos_t otadata, record;
} layout_t;

// The partition table, read the way the bootloader's own loader reads it
// (bootloader_utility_load_partition_table), without printing it twice.
static bool read_layout(layout_t *l)
{
    memset(l, 0, sizeof *l);
    const esp_partition_info_t *t = bootloader_mmap(ESP_PARTITION_TABLE_OFFSET,
                                                    ESP_PARTITION_TABLE_MAX_LEN);
    if (!t) return false;
    int n = 0;
    bool ok = esp_partition_table_verify(t, true, &n) == ESP_OK;
    for (int i = 0; ok && i < n; i++) {
        const esp_partition_info_t *p = &t[i];
        if (p->type == PART_TYPE_APP &&
            (p->subtype & ~PART_SUBTYPE_OTA_MASK) == PART_SUBTYPE_OTA_FLAG) {
            l->ota[p->subtype & PART_SUBTYPE_OTA_MASK] = p->pos;
            l->nslots++;
        } else if (p->type == PART_TYPE_DATA && p->subtype == PART_SUBTYPE_DATA_OTA) {
            l->otadata = p->pos;
        } else if (p->type == PART_TYPE_DATA &&
                   !strncmp((const char *)p->label, KG_PART_LABEL, sizeof p->label)) {
            l->record = p->pos;
        }
    }
    bootloader_munmap(t);
    return ok;
}

// The first bytes of the app_elf_sha256 in the image's description, which
// is what the record names. False when the slot holds no image.
static bool image_sha(const esp_partition_pos_t *p, uint8_t sha[KG_SHA_LEN])
{
    esp_app_desc_t d;
    uint32_t at = p->offset + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (!p->size || bootloader_flash_read(at, &d, sizeof d, true) != ESP_OK ||
        d.magic_word != ESP_APP_DESC_MAGIC_WORD)
        return false;
    memcpy(sha, d.app_elf_sha256, KG_SHA_LEN);
    return true;
}

// The known-good slot, or -1 when the record does not vouch for what is in
// it now. A missing or unreadable record reads as blank: ota_0.
static int read_good(const layout_t *l)
{
    kg_rec_t two[2];
    memset(two, 0xff, sizeof two);
    if (l->record.size >= 2 * KG_SECTOR) {
        bootloader_flash_read(l->record.offset, &two[0], sizeof two[0], false);
        bootloader_flash_read(l->record.offset + KG_SECTOR, &two[1], sizeof two[1], false);
    }
    int slot = kg_slot(two, l->nslots);
    uint8_t sha[KG_SHA_LEN];
    if (kg_vouches(two, l->nslots, slot, image_sha(&l->ota[slot], sha) ? sha : NULL))
        return slot;
    ESP_LOGW(TAG, "ota_%d no longer holds the known-good image", slot);
    return -1;
}

// Makes `slot` the one the bootloader boots next: a valid entry past both,
// over the sector that does not hold the newest kept entry. Unlike
// esp_ota_set_boot_partition, which overwrites whatever is not active, a cut
// here leaves the kept entry in charge rather than an update on trial, whose
// entry is the active one when this reset is most needed.
static bool point_otadata(const layout_t *l, const kg_ota_t ent[2], int slot)
{
    uint32_t after;
    int sector = kg_ota_write_sector(ent, l->nslots, &after);
    esp_ota_select_entry_t e;
    memset(&e, 0xff, sizeof e);
    e.ota_seq = kg_ota_seq(after, slot, l->nslots);
    e.ota_state = ESP_OTA_IMG_VALID;
    e.crc = bootloader_common_ota_select_crc(&e);
    uint32_t at = l->otadata.offset + sector * KG_SECTOR;

    esp_ota_select_entry_t back;
    bool enc = esp_flash_encryption_enabled();
    return bootloader_flash_erase_sector(at / KG_SECTOR) == ESP_OK &&
           bootloader_flash_write(at, &e, sizeof e, enc) == ESP_OK &&
           bootloader_flash_read(at, &back, sizeof back, enc) == ESP_OK &&
           back.ota_seq == e.ota_seq && back.ota_state == e.ota_state && back.crc == e.crc;
}

// The power-up reset (SPEC.md 3.11): settings erased, and the known-good
// firmware, or failing that any image that checks out, set to boot. The
// bootloader's own slot choice then runs as on any boot.
static void known_good_reset(void)
{
    // The bootloader's watchdog (9 s) has been running through the hold;
    // checking images, erasing and the app's start get a fresh count.
    wdt_hal_context_t wdt = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&wdt);
    wdt_hal_feed(&wdt);
    wdt_hal_write_protect_enable(&wdt);

    ESP_LOGW(TAG, "power-up reset: test button held %d s", RESET_HOLD_S);
    layout_t l;
    if (!read_layout(&l) || l.nslots == 0 || l.nslots > MAX_SLOTS ||
        l.otadata.size < 2 * KG_SECTOR) {
        ESP_LOGE(TAG, "no usable partition table; erasing settings only");
    } else {
        esp_ota_select_entry_t od[2];
        memset(od, 0xff, sizeof od);
        bootloader_common_read_otadata(&l.otadata, od);
        // What otadata says each slot holds: an image someone kept, or one
        // new, on trial or failed, which goes last whatever otadata boots.
        kg_ota_t e[2];
        for (int i = 0; i < 2; i++)
            e[i] = (kg_ota_t){ od[i].ota_seq, od[i].ota_state,
                               od[i].ota_seq != UINT32_MAX &&
                               od[i].crc == bootloader_common_ota_select_crc(&od[i]) };
        int kept;
        unsigned untrusted = kg_ota_untrusted(e, l.nslots, &kept);
        int good = read_good(&l);
        ESP_LOGI(TAG, "known-good: ota_%d, last kept: ota_%d, not kept: 0x%x (-1: none)",
                 good, kept, untrusted);

        int order[MAX_SLOTS], pick = -1;
        int n = kg_reset_order(good, kept, untrusted, l.nslots, order);
        for (int i = 0; i < n && pick < 0; i++) {
            esp_image_metadata_t md;
            if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &l.ota[order[i]], &md) == ESP_OK)
                pick = order[i];
            else
                ESP_LOGW(TAG, "ota_%d holds no valid image", order[i]);
        }
        if (pick < 0)
            ESP_LOGE(TAG, "no valid image in any slot; a USB flash is the way back");
        else if (!point_otadata(&l, e, pick))
            ESP_LOGE(TAG, "could not set ota_%d to boot", pick);
        else
            ESP_LOGW(TAG, "booting ota_%d%s", pick, pick == good ? "" : ", not the known-good slot");
    }
    // After otadata: a cut between the two leaves the right firmware with
    // the old settings, which the next reset finishes.
    if (!bootloader_common_erase_part_type_data("nvs", false))
        ESP_LOGE(TAG, "settings not erased");
#if CONFIG_BOOTLOADER_RESERVE_RTC_MEM
    bootloader_common_set_rtc_retain_mem_factory_reset_state();     // the app's panel cue
#endif
}
#endif

void bootloader_after_init(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    drive(48, 0);       // LED data into U9 (AIRWRITER_LED_DATA_PIN)
    drive(35, 0);       // SP3232 T1IN (AIRWRITER_HSKI_DRV_PIN)
    drive(4, 1);        // the PIC's RA1, TashTalk's UART receive: idle high (AIRWRITER_PIC_CLK_PIN)

    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP &&
        bootloader_common_check_long_hold_gpio_level(RESET_PIN, RESET_HOLD_S, false) == GPIO_LONG_HOLD)
        known_good_reset();
#endif
}
