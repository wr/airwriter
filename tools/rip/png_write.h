// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>
#include <stdio.h>

// 8-bit grayscale, one byte per pixel, row-major. Returns 0 on success.
int png_write_gray(const char *path, const uint8_t *pix, uint32_t w, uint32_t h);
// 8-bit RGB, three bytes per pixel, row-major. Returns 0 on success.
int png_write_rgb(const char *path, const uint8_t *pix, uint32_t w, uint32_t h);
