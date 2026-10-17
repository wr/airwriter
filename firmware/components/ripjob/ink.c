// SPDX-License-Identifier: GPL-3.0-or-later
#include "ink.h"

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
