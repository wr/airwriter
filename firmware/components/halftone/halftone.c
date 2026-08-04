// SPDX-License-Identifier: GPL-3.0-or-later
#include "halftone.h"
#include <stdlib.h>
#include <string.h>

// Bill Atkinson's kernel, written for the Macintosh in the same years the
// ImageWriter shipped. It spreads only 6/8 of the error on purpose, which is
// why it holds contrast on a bilevel device with no ink density control.
static const uint8_t bayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

int ht_init(ht_t *h, ht_algo_t algo, uint32_t width)
{
    memset(h, 0, sizeof *h);
    if (!width) return -1;
    h->algo  = algo;
    h->width = width;
    h->err   = calloc((size_t)width * 3, sizeof *h->err);
    return h->err ? 0 : -1;
}

void ht_free(ht_t *h) { free(h->err); h->err = NULL; }

int ht_algo_from_name(const char *name, ht_algo_t *out)
{
    if (!name) return -1;
    if (!strcmp(name, "threshold")) { *out = HT_THRESHOLD; return 0; }
    if (!strcmp(name, "atkinson"))  { *out = HT_ATKINSON;  return 0; }
    if (!strcmp(name, "floyd"))     { *out = HT_FLOYD;     return 0; }
    if (!strcmp(name, "bayer"))     { *out = HT_BAYER;     return 0; }
    return -1;
}

static int16_t *errrow(ht_t *h, uint32_t y) { return h->err + (size_t)(y % 3) * h->width; }

int16_t ht_peek_err(const ht_t *h, uint32_t ahead, uint32_t x)
{
    return h->err[(size_t)((h->y + ahead) % 3) * h->width + x];
}

static void spread(ht_t *h, uint32_t x, int err)
{
    int16_t *r0 = errrow(h, h->y);
    int16_t *r1 = errrow(h, h->y + 1);
    int16_t *r2 = errrow(h, h->y + 2);
    uint32_t w  = h->width;

    if (h->algo == HT_ATKINSON) {
        int e = err / 8;                    // six neighbours, one eighth each
        if (x + 1 < w) { r0[x + 1] += (int16_t)e; }
        if (x + 2 < w) { r0[x + 2] += (int16_t)e; }
        if (x > 0)     { r1[x - 1] += (int16_t)e; }
        r1[x] += (int16_t)e;
        if (x + 1 < w) { r1[x + 1] += (int16_t)e; }
        r2[x] += (int16_t)e;
    } else {                                // Floyd-Steinberg
        if (x + 1 < w) { r0[x + 1] += (int16_t)(err * 7 / 16); }
        if (x > 0)     { r1[x - 1] += (int16_t)(err * 3 / 16); }
        r1[x] += (int16_t)(err * 5 / 16);
        if (x + 1 < w) { r1[x + 1] += (int16_t)(err * 1 / 16); }
    }
}

void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits)
{
    uint32_t w      = h->width;
    size_t   nbytes = (w + 7) / 8;
    memset(bits, 0, nbytes);

    int16_t *cur = errrow(h, h->y);

    for (uint32_t x = 0; x < w; x++) {
        int v = gray[x];
        int ink;

        switch (h->algo) {
        case HT_THRESHOLD:
            ink = v < 128;
            break;
        case HT_BAYER: {
            int t = (bayer8[h->y % 8][x % 8] * 255) / 64;
            ink = v <= t;
            break;
        }
        default: {                              // HT_ATKINSON, HT_FLOYD
            int adj = v + cur[x];
            if (adj < 0)   adj = 0;
            if (adj > 255) adj = 255;
            int out = adj < 128 ? 0 : 255;
            ink = (out == 0);
            spread(h, x, adj - out);
            break;
        }
        }

        if (ink) bits[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }

    // This row's error slot is finished with; clear it for reuse two rows on.
    memset(cur, 0, (size_t)w * sizeof *cur);
    h->y++;
}
