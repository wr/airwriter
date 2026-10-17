// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grayscale to 1bpp. Input is luminance, 0 = black. Output bit set = ink.
//
// Two kinds of screen. The error-diffusion ones (Atkinson, Floyd-Steinberg)
// carry what a pixel could not print to its neighbours and scan serpentine,
// each row the other way, so no diagonal worm can form. The mask screens
// (Bayer, blue noise) compare each pixel with a fixed tile and carry nothing,
// so type stays exactly where it was drawn; the blue-noise tile has no weave.
//
// A dot on this printer is as wide as its row, so neighbours overlap and a
// tint prints darker than its bits: ht_set_gain puts a tone curve ahead of
// every screen that asks for fewer dots in the midtones to make up for it.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER, HT_BLUENOISE } ht_algo_t;

typedef struct {
    ht_algo_t algo;
    uint32_t  width;
    int16_t  *err;      // 3 rows of width, used as a ring by row index % 3
    uint32_t  y;
    uint8_t   tone[256];    // the curve every gray goes through; identity until ht_set_gain
    uint32_t  phase_x, phase_y; // where in its tile a mask screen starts
    int       invert;       // mask screen read upside down (255 - t)
} ht_t;

int  ht_init(ht_t *h, ht_algo_t algo, uint32_t width);
void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits);
void ht_free(ht_t *h);
int  ht_algo_from_name(const char *name, ht_algo_t *out);

// Dot gain at a 50% tint, in percent: 20 means a tint asked at half prints
// at seventy. The curve asks for the number of dots that will read right
// once they have spread; 0 and 255 stay themselves. 0 removes the curve.
void ht_set_gain(ht_t *h, int gain_pct);

// Mask screens only: start the tile dx, dy in, and read it upside down if
// asked. Two inks on the same tile upside down to each other never share a
// dot in a light tint; two on tiles far apart share them only by chance.
void ht_set_phase(ht_t *h, uint32_t dx, uint32_t dy, int invert);

// As ht_row, but where `avoid` has a bit set (an earlier ink's dot) this
// pixel has to be a quarter darker to print; what it then does not print is
// carried on by the error diffusion, so the tint stays right and the dots
// spread out. Mask screens keep inks apart by phase and ignore it. NULL
// avoid is ht_row.
void ht_row_avoid(ht_t *h, const uint8_t *gray, const uint8_t *avoid, uint8_t *bits);

// Test hooks: error carried for a row `ahead` rows from now, at column x; the
// blue-noise tile at x, y.
int16_t ht_peek_err(const ht_t *h, uint32_t ahead, uint32_t x);
uint8_t ht_bluenoise_at(uint32_t x, uint32_t y);
