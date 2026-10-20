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
    char queue[SET_QUEUE_MAX];       // the printer's name everywhere: name, else model
    char model[SET_QUEUE_MAX];       // the printer model picked on the page (W-1079)
    char name[SET_QUEUE_MAX];        // a name of the user's own, or "" for the model's
    char dither[SET_DITHER_MAX];     // atkinson | floyd | bayer | bluenoise | threshold
    uint8_t gain;                    // dot gain at a 50% tint the screens make up for,
                                     // percent (W-1068); CONFIG_AIRWRITER_DOT_GAIN
    uint8_t auto_update;             // install new releases by itself (W-1048); off
    uint8_t ribbon;                  // the last ribbon self-ID reported (W-1064):
                                     // 0 unknown, 1 black, 2 colour (iw_ribbon_t)
    uint8_t airprint;                // advertise and take AirPrint jobs (W-1079); on
    int8_t  lt_port;                 // the AppleTalk port (W-1079): 1 on, 0 off, -1 as
                                     // DIP switches 1-2 say (mode 2 on), until set
} settings_t;

// Fills every field: NVS first, then the Kconfig default, then a built-in.
// `mac_suffix` is the four hex digits used for the default hostname.
esp_err_t settings_load(settings_t *out, const char *mac_suffix);

esp_err_t settings_save_wifi(const char *ssid, const char *pass);
// Whether Wi-Fi was saved before this restart, once: the panel greets the
// first join with green as confirmation (W-1049). Kept in RTC memory, so it
// survives the restart that follows the save, not a power cut.
int       settings_take_wifi_fresh(void);
// Empty strings and a gain below 0 keep what is stored.
esp_err_t settings_save_general(const char *hostname, const char *queue,
                                const char *dither, int gain);
esp_err_t settings_save_auto_update(int on);
// The printer's model and the user's own name for it (W-1079). NULL keeps
// each; an empty name goes back to the model's. Also stores the name in
// effect as "queue", which firmware from before W-1079 reads.
esp_err_t settings_save_printer(const char *model, const char *name);
// AirPrint and the AppleTalk port, on (1) or off (0); -1 keeps each.
esp_err_t settings_save_switches(int airprint, int lt_port);
// Kept so a restart advertises what the printer last said, not black until it
// answers again: what clients see must not flip back and forth (W-1064).
esp_err_t settings_save_ribbon(uint8_t ribbon);

// Wipes the stored settings. The device falls back to its build defaults on
// the next boot, which is what the config button holds for.
esp_err_t settings_erase(void);
