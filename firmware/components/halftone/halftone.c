// SPDX-License-Identifier: GPL-3.0-or-later
#include "halftone.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "bluenoise64.h"

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
    for (int v = 0; v < 256; v++) h->tone[v] = (uint8_t)v;
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
    if (!strcmp(name, "bluenoise")) { *out = HT_BLUENOISE; return 0; }
    return -1;
}

// A dot's printed area over its cell's, as a power: dots laid at random
// cover 1 - (1 - a)^k of the paper for a of them per cell. The gain asked
// for at a half tint fixes k, and the curve is that law run backwards, so
// the dots asked for are the ones that will read right once they have
// spread. Gray is paper-fraction, so the curve lifts the midtones.
void ht_set_gain(ht_t *h, int gain_pct)
{
    if (gain_pct <= 0 || gain_pct >= 50) {
        for (int v = 0; v < 256; v++) h->tone[v] = (uint8_t)v;
        return;
    }
    double k = log(1.0 / (0.5 - gain_pct / 100.0)) / log(2.0);
    for (int v = 0; v < 256; v++) {
        double t = 255.0 * pow(v / 255.0, 1.0 / k) + 0.5;
        h->tone[v] = (uint8_t)(t > 255 ? 255 : t);
    }
    h->tone[0] = 0;
    h->tone[255] = 255;
}

void ht_set_phase(ht_t *h, uint32_t dx, uint32_t dy, int invert)
{
    h->phase_x = dx;
    h->phase_y = dy;
    h->invert  = invert;
}

uint8_t ht_bluenoise_at(uint32_t x, uint32_t y)
{
    return bluenoise64[(y & 63) * 64 + (x & 63)];
}

static int16_t *errrow(ht_t *h, uint32_t y) { return h->err + (size_t)(y % 3) * h->width; }

int16_t ht_peek_err(const ht_t *h, uint32_t ahead, uint32_t x)
{
    return h->err[(size_t)((h->y + ahead) % 3) * h->width + x];
}

// One tap of the kernel: column x + dx of the given error row, if it exists.
static inline void tap(int16_t *row, uint32_t x, int dx, uint32_t w, int e)
{
    int64_t c = (int64_t)x + dx;
    if (c >= 0 && c < (int64_t)w) row[c] += (int16_t)e;
}

// dir is +1 on a row scanned left to right and -1 on one scanned back, and
// the kernel is mirrored with it: "ahead" is always the way the scan goes.
static void spread(ht_t *h, uint32_t x, int err, int dir)
{
    int16_t *r0 = errrow(h, h->y);
    int16_t *r1 = errrow(h, h->y + 1);
    int16_t *r2 = errrow(h, h->y + 2);
    uint32_t w  = h->width;

    if (h->algo == HT_ATKINSON) {
        int e = err / 8;                    // six neighbours, one eighth each
        tap(r0, x,  dir,     w, e);
        tap(r0, x,  2 * dir, w, e);
        tap(r1, x, -dir,     w, e);
        tap(r1, x,  0,       w, e);
        tap(r1, x,  dir,     w, e);
        tap(r2, x,  0,       w, e);
    } else {                                // Floyd-Steinberg
        tap(r0, x,  dir, w, err * 7 / 16);
        tap(r1, x, -dir, w, err * 3 / 16);
        tap(r1, x,  0,   w, err * 5 / 16);
        tap(r1, x,  dir, w, err * 1 / 16);
    }
}

void ht_row(ht_t *h, const uint8_t *gray, uint8_t *bits)
{
    ht_row_avoid(h, gray, NULL, bits);
}

void ht_row_avoid(ht_t *h, const uint8_t *gray, const uint8_t *avoid, uint8_t *bits)
{
    uint32_t w      = h->width;
    size_t   nbytes = (w + 7) / 8;
    memset(bits, 0, nbytes);

    int16_t *cur = errrow(h, h->y);
    int diffusing = h->algo == HT_ATKINSON || h->algo == HT_FLOYD;
    int dir = (diffusing && (h->y & 1)) ? -1 : 1;
    uint32_t ty = h->y + h->phase_y;

    for (uint32_t i = 0; i < w; i++) {
        uint32_t x = dir > 0 ? i : w - 1 - i;
        int v = h->tone[gray[x]];
        int ink;

        switch (h->algo) {
        case HT_THRESHOLD:
            ink = v < 128;
            break;
        case HT_BAYER: {
            int t = (bayer8[ty % 8][(x + h->phase_x) % 8] * 255) / 64;
            if (h->invert) t = 251 - t;
            ink = v <= t;
            break;
        }
        case HT_BLUENOISE: {
            // 16 cells of the tile at each level, so a flat v prints
            // (256 - v) / 256 of them; bare paper prints none at all.
            int t = ht_bluenoise_at(x + h->phase_x, ty);
            if (h->invert) t = 255 - t;
            ink = v < 255 && v <= t;
            break;
        }
        default: {                              // HT_ATKINSON, HT_FLOYD
            // The published algorithms derive the quantisation error from the
            // unclamped accumulated value. Clamping here instead would discard
            // error whenever a pixel overshoots, which lightens or darkens the
            // result and loses detail in extremes.
            int raw = v + cur[x];
            int cut = (avoid && (avoid[x >> 3] & (0x80 >> (x & 7)))) ? 64 : 128;
            int out = raw < cut ? 0 : 255;
            ink = (out == 0);
            spread(h, x, raw - out, dir);
            break;
        }
        }

        if (ink) bits[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }

    // This row's error slot is finished with; clear it for reuse two rows on.
    memset(cur, 0, (size_t)w * sizeof *cur);
    h->y++;
}
