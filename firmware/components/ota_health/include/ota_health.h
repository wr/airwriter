// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the rollback health gate (W-1046) and the known-good slot
// (W-1059). The decisions are the pure code in ota_health_core.h and
// known_good_core.h; this reads the running image's OTA state, feeds the gate
// the clock and marks the image valid, or invalid and reboots, and moves the
// known-good record.
#pragma once
#include <stdint.h>
#include "esp_partition.h"
#include "ota_health_core.h"

// If the running image is on trial (a first boot after an OTA), starts the
// gate's task; otherwise only records the slot for the status page. Reads the
// known-good record. Call once, early in app_main, when it is known whether
// the board has Wi-Fi credentials.
void ota_health_start(int has_creds);

// An OH_* bit came up or went down. Any task; a no-op when nothing is on trial.
void ota_health_set(uint32_t bits, int on);

// A real print finished: an IPP or LocalTalk job that ran to its end, every
// byte taken by the printer. Once the image is also kept, its slot becomes
// the known-good one (one flash write). Call holding the job lock, so it
// never runs beside an update choosing its slot.
void ota_health_printed(void);

// Where an update goes: never the running slot or the known-good one. NULL
// when there is none. On a board without the W-1059 table, IDF's next slot.
const esp_partition_t *ota_health_update_target(void);

const char *ota_health_slot(void);          // "ota_0", "ota_1", "ota_2"
int         ota_health_on_trial(void);      // still deciding about this image
// The version of the image the bootloader last rolled back from, or "".
const char *ota_health_rolled_back(void);
// The slot the power-up reset boots ("ota_1"), and the version in it ("" if
// it holds no valid image). Both "" on a board without the W-1059 table.
const char *ota_health_known_good(void);
const char *ota_health_known_good_version(void);
