// SPDX-License-Identifier: GPL-3.0-or-later
//
// RGB to the colour ribbon's four inks (W-1064).
//
// The ribbon's bands are subtractive: cyan takes out red, magenta green,
// yellow blue. Whatever all three would print in common goes to the black
// band instead (full grey-component replacement), so greys and black text
// print in one pass, with the cleanest black and no wear on the colour bands.
// Tune here after a test print if colours come out too dark or too thin.
#pragma once
#include <stdint.h>
#include "imagewriter.h"
#include "halftone.h"

// Planes are in halftone's convention, one byte per pixel: 255 is no ink and
// 0 is full ink. planes[] is indexed by iw_ink_t, each w bytes.
void ink_split(const uint8_t *rgb, uint32_t w, uint8_t *const planes[IW_INKS]);

// RGB to the gray a black ribbon prints: Rec. 601 luma, white and black exact.
void ink_gray(const uint8_t *rgb, uint32_t w, uint8_t *gray);

// A colour row through the screens, one per ink, the way every colour page
// prints: black first so it stays exact and the colours keep off it, then
// cyan, magenta and yellow, each where an earlier ink left room (dot-off-
// dot, ht_row_avoid). The mask screens sit black and cyan on one tile upside
// down to each other and magenta and yellow on a tile far away, so the pairs
// that make the common tints never share a dot until they have to. Apple's
// pattern screen (HT_APPLE) is a mask screen too, with its own placement: cyan,
// magenta and yellow read its tile as it is and black reads it turned over
// (ink_screen_phase).
typedef struct {
    ht_t     ht[IW_INKS];
    uint8_t *plane[IW_INKS];
    uint8_t *avoid;
    uint32_t w;
    const uint8_t *tone;        // ink_screens_set_tone's curve, or NULL
} ink_screens_t;

// 0, or -1 if out of memory (nothing is left allocated).
int  ink_screens_init(ink_screens_t *s, ht_algo_t algo, int gain, uint32_t w);
// A tone curve put on R, G and B before each pixel is split into inks (the
// StyleWriter drivers' GAMA curves, apple_tables.h): lightness in, lightness
// out, 0 black to 255 white. NULL, the way it starts, is none. The curve is
// kept, not copied, so it has to outlive the screens.
void ink_screens_set_tone(ink_screens_t *s, const uint8_t curve[256]);
// One RGB row of w pixels to a 1bpp row per ink, indexed by iw_ink_t.
void ink_screens_row(ink_screens_t *s, const uint8_t *rgb, uint8_t *const bits[IW_INKS]);
void ink_screens_free(ink_screens_t *s);
// The ink's place on the mask tile, for a halftoner that prints one ink alone.
// h->algo says which tile: the 64-pixel blue-noise and Bayer tiles are shared out
// so that black and cyan, and magenta and yellow, are upside down to each other.
// Apple's 16 x 16 starts at its corner for every ink and turns only black over;
// a job that wants the drivers' row to start on (the page's first row is the
// tile's second) sets it with ht_set_phase.
void ink_screen_phase(ht_t *h, iw_ink_t ink);
