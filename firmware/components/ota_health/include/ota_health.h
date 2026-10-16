// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the rollback health gate (W-1046). The decision is the pure
// code in ota_health_core.h; this reads the running image's OTA state, feeds
// the gate the clock and marks the image valid, or invalid and reboots.
#pragma once
#include <stdint.h>
#include "ota_health_core.h"

// If the running image is on trial (a first boot after an OTA), starts the
// gate's task; otherwise only records the slot for the status page. Call once,
// early in app_main, when it is known whether the board has Wi-Fi credentials.
void ota_health_start(int has_creds);

// An OH_* bit came up or went down. Any task; a no-op when nothing is on trial.
void ota_health_set(uint32_t bits, int on);

const char *ota_health_slot(void);          // "factory", "ota_0", "ota_1"
int         ota_health_on_trial(void);      // still deciding about this image
// The version of the image the bootloader last rolled back from, or "".
const char *ota_health_rolled_back(void);
