// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grayscale to 1bpp. Input is luminance, 0 = black. Output bit set = ink.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef enum { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER } ht_algo_t;

typedef struct {
    ht_algo_t algo;
    uint32_t  width;
    int16_t  *err;      // 3 rows of width, used as a ring by row index % 3
    uint32_t  y;
} ht_t;

int  ht_init(ht_t *h, ht_algo_t algo, uint32_t width);
void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits);
void ht_free(ht_t *h);
int  ht_algo_from_name(const char *name, ht_algo_t *out);

// Test hook: error carried for a row `ahead` rows from now, at column x.
int16_t ht_peek_err(const ht_t *h, uint32_t ahead, uint32_t x);
