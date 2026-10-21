// SPDX-License-Identifier: GPL-3.0-or-later
#include "ink.h"
#include <stdlib.h>
#include <string.h>

void ink_split(const uint8_t *rgb, uint32_t w, uint8_t *const planes[IW_INKS])
{
    for (uint32_t x = 0; x < w; x++, rgb += 3) {
        uint8_t r = rgb[0], g = rgb[1], b = rgb[2];
        uint8_t mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        // Black is 255 - mx of ink, so its plane is mx; each colour keeps
        // only what black did not already print.
        planes[IW_BLACK][x]   = mx;
        planes[IW_CYAN][x]    = (uint8_t)(255 - (mx - r));
        planes[IW_MAGENTA][x] = (uint8_t)(255 - (mx - g));
        planes[IW_YELLOW][x]  = (uint8_t)(255 - (mx - b));
    }
}

void ink_gray(const uint8_t *rgb, uint32_t w, uint8_t *gray)
{
    for (uint32_t x = 0; x < w; x++, rgb += 3)
        gray[x] = (uint8_t)((77u * rgb[0] + 150u * rgb[1] + 29u * rgb[2] + 128u) >> 8);
}

static const iw_ink_t ink_order[IW_INKS] = { IW_BLACK, IW_CYAN, IW_MAGENTA, IW_YELLOW };
static const struct { uint32_t dx, dy; int invert; } ink_phase[IW_INKS] = {
    [IW_BLACK]   = {  0,  0, 0 },
    [IW_CYAN]    = {  0,  0, 1 },
    [IW_MAGENTA] = { 36, 36, 0 },
    [IW_YELLOW]  = { 36, 36, 1 },
};

void ink_screen_phase(ht_t *h, iw_ink_t ink)
{
    ht_set_phase(h, ink_phase[ink].dx, ink_phase[ink].dy, ink_phase[ink].invert);
}

int ink_screens_init(ink_screens_t *s, ht_algo_t algo, int gain, uint32_t w)
{
    memset(s, 0, sizeof *s);
    s->w = w;
    int ready = 0;
    for (; ready < IW_INKS; ready++) {
        s->plane[ready] = malloc(w);
        if (!s->plane[ready] || ht_init(&s->ht[ready], algo, w)) break;
        ht_set_gain(&s->ht[ready], gain);
        ink_screen_phase(&s->ht[ready], (iw_ink_t)ready);
    }
    s->avoid = ready == IW_INKS ? calloc((w + 7) / 8, 1) : NULL;
    if (s->avoid) return 0;
    // A half-built set: ht_free and free both take what was never made.
    for (int i = 0; i < IW_INKS; i++) {
        if (i < ready) ht_free(&s->ht[i]);
        free(s->plane[i]);
    }
    memset(s, 0, sizeof *s);
    return -1;
}

void ink_screens_row(ink_screens_t *s, const uint8_t *rgb, uint8_t *const bits[IW_INKS])
{
    ink_split(rgb, s->w, s->plane);
    size_t nb = (s->w + 7) / 8;
    memset(s->avoid, 0, nb);
    for (int k = 0; k < IW_INKS; k++) {
        iw_ink_t i = ink_order[k];
        ht_row_avoid(&s->ht[i], s->plane[i], s->avoid, bits[i]);
        for (size_t b = 0; b < nb; b++) s->avoid[b] |= bits[i][b];
    }
}

void ink_screens_free(ink_screens_t *s)
{
    for (int i = 0; i < IW_INKS; i++) {
        ht_free(&s->ht[i]);
        free(s->plane[i]);
    }
    free(s->avoid);
    memset(s, 0, sizeof *s);
}
