// SPDX-License-Identifier: GPL-3.0-or-later
//
// Everything a user can change, stored in NVS. The Kconfig values are only
// build-time defaults: whatever is in NVS wins, so a device can be
// reconfigured from the web UI without a rebuild.
#pragma once
#include <stdint.h>
#include "esp_err.h"

#define SET_SSID_MAX  33
#define SET_PASS_MAX  65
#define SET_HOST_MAX  40
#define SET_QUEUE_MAX 32
#define SET_DITHER_MAX 12

typedef struct {
    char ssid[SET_SSID_MAX];
    char pass[SET_PASS_MAX];
    char hostname[SET_HOST_MAX];     // without .local
    char queue[SET_QUEUE_MAX];       // what the print sheet shows
    char dither[SET_DITHER_MAX];     // atkinson | floyd | bayer | threshold
    uint8_t auto_update;             // install new releases by itself (W-1048); off
} settings_t;

// Fills every field: NVS first, then the Kconfig default, then a built-in.
// `mac_suffix` is the four hex digits used for the default hostname.
esp_err_t settings_load(settings_t *out, const char *mac_suffix);

esp_err_t settings_save_wifi(const char *ssid, const char *pass);
// Whether Wi-Fi was saved before this restart, once: the panel greets the
// first join with green as confirmation (W-1049). Kept in RTC memory, so it
// survives the restart that follows the save, not a power cut.
int       settings_take_wifi_fresh(void);
esp_err_t settings_save_general(const char *hostname, const char *queue,
                                const char *dither);
esp_err_t settings_save_auto_update(int on);

// Wipes the stored settings. The device falls back to its build defaults on
// the next boot, which is what the config button holds for.
esp_err_t settings_erase(void);
