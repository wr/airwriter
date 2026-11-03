// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for FreeRTOS task.h: a delay moves the test's clock.
#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
