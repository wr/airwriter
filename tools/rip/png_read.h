// SPDX-License-Identifier: GPL-3.0-or-later
//
// A PNG page for the host tools (W-1103, swhost): decoded with zlib and handed on as a
// one-page URF held in memory, so the rip reads it as it reads any page. 8-bit gray or
// 24-bit sRGB, 360 dpi unless the file says otherwise.
#pragma once
#include <stdint.h>
#include <stddef.h>

// "\x89PNG\r\n\x1a\n" starts n bytes.
int png_is_png(const uint8_t *b, size_t n);

// Decodes `png` (n bytes) into a malloc'd URF, *urf and *urf_n. Gray, gray with alpha and a
// palette of grays become an 8-bit gray page; RGB, RGBA and a palette with any colour a
// 24-bit sRGB one. Alpha is laid over white. Bit depths 1 to 16, not interlaced. The page's
// dpi is the file's pHYs (pixels per metre, rounded to whole dpi), or default_dpi. Returns
// 0, or -1 with *why a short reason (a static string) and nothing allocated.
int png_to_urf(const uint8_t *png, size_t n, uint32_t default_dpi, uint8_t **urf, size_t *urf_n,
               const char **why);
