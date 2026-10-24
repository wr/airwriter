// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page for a PostScript printer (W-1088, printers design, "Test
// pages"): what the ImageWriter's test page is for the ImageWriter, set in the
// printer's own Times and Helvetica. The AirWriter mark, the printer's name
// and product, the board's details, a type sample, a gray ramp from the
// printer's own screen, AirWriter's five screens as 300 dpi imagemask
// swatches, a ruler and four one-inch squares. One Letter page of plain Level
// 1 PostScript, hex data only: it goes to the printer as it is, no rip, no
// query needed first. Pure C; pscheck.py renders it under the Level 1 trap.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *printer;         // its name in the list (UTF-8)
    const char *product;         // its query's product, or ""
    const char *version;         // its PostScript version, or ""
    const char *host;            // "airwriter-2434.local"
    const char *board;           // "AirWriter v0.3.0 at 10.0.1.115"
} ps_tp_info_t;

// 0, or -1 when a screen's buffers could not be had (the page is cut short).
int ps_testpage(const ps_tp_info_t *i, void (*out)(void *ctx, const uint8_t *b, size_t n), void *ctx);
