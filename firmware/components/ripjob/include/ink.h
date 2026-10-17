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

// Planes are in halftone's convention, one byte per pixel: 255 is no ink and
// 0 is full ink. planes[] is indexed by iw_ink_t, each w bytes.
void ink_split(const uint8_t *rgb, uint32_t w, uint8_t *const planes[IW_INKS]);

// RGB to the gray a black ribbon prints: Rec. 601 luma, white and black exact.
void ink_gray(const uint8_t *rgb, uint32_t w, uint8_t *gray);
