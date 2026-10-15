// SPDX-License-Identifier: GPL-3.0-or-later
// Pure frame renderer for the 37-dot art panel: (view, options, t) -> one RGB
// frame. No FreeRTOS, no I/O -- led_grid.c drives the RMT strip from its output.
#pragma once
#include <stdint.h>
#include "led_map.h"
#include "printer_state_core.h"

#define LG_BRIGHTNESS_CAP 48    // of 255, FR4 dots -- no hardware current limit on rev A
#define LG_MASK_CAP       192   // mask dots in the all-dot states: 4x, they pass ~10% / ~5%
#define LG_ALERT_CAP      96    // FR4 dots in Needs attention: it has to read across the room
#define LG_BOOT_MS        3000  // the boot spin; main sets printer_state's boot_ms to this

typedef struct {
    uint8_t gamer;              // DIP7: Ready and Asleep drift through RGB instead of white
} lg_opts_t;

void lg_render(const ps_view_t *v, const lg_opts_t *o, uint32_t t_ms,
               uint8_t out[LG_NUM_LEDS][3]);
int  lg_pilot_index(void);      // the FR4 dot right of the test button (row 3, col 4)

// The same frame at 8.8 fixed point (value x 256). Slow fades at a few counts
// of drive need the fraction: an 8-bit Ready breath has only ~6 levels, and
// each step is a visible jump. lg_render() is this, rounded.
void lg_render16(const ps_view_t *v, const lg_opts_t *o, uint32_t t_ms,
                 uint16_t out[LG_NUM_LEDS][3]);

// Sigma-delta down to the LEDs' 8 bits: each channel carries its rounding
// error into the next frame, so over time the output averages to the 16-bit
// level. Zero-initialise once; call once per frame.
typedef struct { uint8_t err[LG_NUM_LEDS][3]; } lg_dither_t;
void lg_dither(lg_dither_t *d, const uint16_t in[LG_NUM_LEDS][3],
               uint8_t out[LG_NUM_LEDS][3]);
