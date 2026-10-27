// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for FreeRTOS.h: 100 ticks a second, as the board runs.
#pragma once
#include <stdint.h>
typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms)) * 100u / 1000u))
