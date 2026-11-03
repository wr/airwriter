// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for ESP-IDF's esp_timer.h: the test owns the clock.
#pragma once
#include <stdint.h>
int64_t esp_timer_get_time(void);
