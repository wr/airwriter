// SPDX-License-Identifier: GPL-3.0-or-later
// How hot the ESP32-S3 runs: its own sensor, read once a second by the main
// task, with the highest reading since boot. /api/status shows both.
//
// The sensor is on the die, so it reads above the air in the case and above
// the module's can, which is what a module's rating is for (SPEC §3.19). A
// bench diagnostic: nothing acts on the number.
#pragma once
#include <stddef.h>

// Installs the sensor and takes the first reading. Once, in app_main. A board
// whose sensor will not install runs on without one.
void chip_temp_start(void);

// One reading. From one task only (the main task's loop): the driver's range
// change is not locked against a second caller.
void chip_temp_sample(void);

// The JSON for /api/status, a leading comma included: `,"temp":41.5,"temp_max":47.2`,
// in °C, each null until there is a reading. Any task. Returns the length written.
size_t chip_temp_json(char *buf, size_t cap);
