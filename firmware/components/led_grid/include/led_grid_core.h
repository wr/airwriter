// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include "led_map.h"
#include "printer_state_core.h"
#define LG_BRIGHTNESS_CAP 48   // of 255 -- no hardware current limit on rev A
void lg_render(ps_state_t st, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3]);
