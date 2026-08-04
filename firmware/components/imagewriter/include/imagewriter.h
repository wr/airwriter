// SPDX-License-Identifier: GPL-3.0-or-later
//
// ImageWriter II output stage. 144 dpi horizontal, 72 dpi vertical, one pass.
//
// Escape codes from the Apple ImageWriter II Technical Reference:
//   ESC c        reset                              (Table A-15, p. 144)
//   ESC p        144 dpi, 1152 dots max             (Table 8-2,  p. 106)
//   ESC T nn     line feed of nn/144 inch, 2 digits (Table A-15, p. 144)
//   ESC G nnnn   nnnn bytes of graphics, 4 digits   (Table 8-1,  p. 105)
// Bit 0 of a graphics byte is the TOP pin (p. 107).
// ESC T16 is 16/144 = 8/72 inch, which exactly abuts successive strips.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define IW_MAX_DOTS 1152

typedef void (*iw_out_fn)(void *ctx, const uint8_t *buf, size_t n);

typedef struct {
    uint32_t  width_dots;
    iw_out_fn out;
    void     *ctx;
    uint8_t   rows[8][(IW_MAX_DOTS + 7) / 8];
    uint32_t  nrows;
    uint8_t   cols[IW_MAX_DOTS];
} iw_t;

void iw_init(iw_t *w, uint32_t width_dots, iw_out_fn out, void *ctx);
void iw_begin_page(iw_t *w);
void iw_row(iw_t *w, const uint8_t *bits);
void iw_end_page(iw_t *w);
void iw_transpose(const uint8_t *rows[8], uint8_t *cols, uint32_t width_dots);
