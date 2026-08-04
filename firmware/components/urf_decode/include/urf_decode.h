// SPDX-License-Identifier: GPL-3.0-or-later
//
// URF (Apple Raster) and PWG Raster decoder.
//
// Binary layout and the line encoding were read from OpenPrinting/cups
// cups/raster-stream.c (_cupsRasterReadHeader, _cupsRasterReadPixels) and
// PWG 5102.4-2012 section 4.4. Do not "fix" the arithmetic from memory:
// the three +1 conventions in the line encoding are deliberate.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define URF_OK               0
#define URF_END              1
#define URF_ERR_IO          -1
#define URF_ERR_MAGIC       -2
#define URF_ERR_UNSUPPORTED -3
#define URF_ERR_MALFORMED   -4

// Returns 1 when all n bytes were read, 0 on short read or error.
typedef int (*urf_read_fn)(void *ctx, uint8_t *buf, size_t n);

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t res_x;           // dpi; URF carries one value, copied to both
    uint32_t res_y;
    uint8_t  bits_per_pixel;
    uint8_t  colorspace;      // raw code from the file
    uint8_t  num_colors;
    uint8_t  bits_per_color;
    uint8_t  bpp_bytes;       // width of one colour value, the RLE run unit
    uint8_t  fill_byte;       // what control byte 0x80 fills with
    size_t   bytes_per_line;
} urf_page_t;

typedef struct {
    urf_read_fn read;
    void       *ctx;
    int         is_pwg;
    uint32_t    pages_total;  // 0 means unknown / streaming
    uint32_t    pages_seen;
    urf_page_t  page;
    uint8_t    *line;         // decoded row scratch, bytes_per_line
    size_t      line_cap;
    unsigned    repeat_left;  // rows still owed from the line-repeat byte
    uint32_t    rows_emitted;
} urf_t;

int  urf_open(urf_t *u, urf_read_fn read, void *ctx);
int  urf_next_page(urf_t *u);
int  urf_next_row(urf_t *u, const uint8_t **row);
void urf_free(urf_t *u);
