// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page's still life: four glossy balls on a checkered floor under a
// blue sky, ray traced rather than stored, so it costs no flash and has the
// smooth tone, highlights and soft shadow a halftone screen shows itself on.
// Laid out in inches and sampled on the printer's own grid, 1/144 in across
// and 1/72 in down (tp_scene_row), or square (tp_scene_row_aspect), so a w x h
// picture prints w/144 by h/72 inches, or at the aspect it is given.
#pragma once
#include <stdint.h>

// Row y of a w x h picture: w pixels of sRGB, 3 bytes each.
void tp_scene_row(uint32_t w, uint32_t h, uint32_t y, uint8_t *rgb);
// The same for a picture whose height over its width, in inches, is `aspect`: the
// ImageWriter's w x h pixels are w/144 by h/72 in, a square-pixel page's (a StyleWriter's,
// 360 x 360 dpi) are h/w.
void tp_scene_row_aspect(uint32_t w, uint32_t h, uint32_t y, uint8_t *rgb, float aspect);
