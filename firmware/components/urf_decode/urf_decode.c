// SPDX-License-Identifier: GPL-3.0-or-later
#include "urf_decode.h"
#include <stdlib.h>
#include <string.h>

// cups/raster-stream.c, cups_raster_read_header(): rawcspace[] / rawnumcolors[]
static uint8_t colorspace_num_colors(uint8_t code)
{
    static const uint8_t n[] = { 1, 3, 3, 3, 1, 3, 4 };
    return code < sizeof n ? n[code] : 1;
}

// cups/raster-stream.c, _cupsRasterReadPixels(): control byte 0x80 fills 0xFF
// for additive spaces (W, RGB, SW, SRGB, RGBW, AdobeRGB) and 0x00 otherwise.
// URF codes: 0 SW, 1 SRGB, 3 AdobeRGB, 4 W, 5 RGB are additive; 2 CIELab and
// 6 CMYK are not. Getting this backwards prints solid black pages.
static uint8_t colorspace_fill(uint8_t code)
{
    switch (code) {
    case 0: case 1: case 3: case 4: case 5: return 0xFF;
    default:                                return 0x00;
    }
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

int urf_open(urf_t *u, urf_read_fn read, void *ctx)
{
    memset(u, 0, sizeof *u);
    u->read = read;
    u->ctx  = ctx;

    uint8_t h[12];
    if (!read(ctx, h, 12)) return URF_ERR_IO;

    if (memcmp(h, "UNIRAST\0", 8) == 0) {
        u->is_pwg      = 0;
        u->pages_total = be32(h + 8);
        return URF_OK;
    }
    if (memcmp(h, "RaS2", 4) == 0) {
        // PWG: the sync word is 4 bytes, so 8 of the 12 read belong to the
        // first page header. Task 3 handles PWG; reject it for now rather
        // than silently mis-parsing.
        u->is_pwg = 1;
        return URF_ERR_UNSUPPORTED;
    }
    return URF_ERR_MAGIC;
}

int urf_next_page(urf_t *u)
{
    if (u->pages_total && u->pages_seen >= u->pages_total) return URF_END;

    uint8_t h[32];
    if (!u->read(u->ctx, h, 32)) {
        // A streaming file with pages_total == 0 ends by running out.
        return u->pages_total ? URF_ERR_IO : URF_END;
    }

    urf_page_t *p = &u->page;
    p->bits_per_pixel = h[0];
    p->colorspace     = h[1];
    p->width          = be32(h + 12);
    p->height         = be32(h + 16);
    p->res_x          = be32(h + 20);
    p->res_y          = p->res_x;          // URF carries a single scalar

    if (p->bits_per_pixel == 0 || p->width == 0 || p->height == 0)
        return URF_ERR_UNSUPPORTED;

    p->num_colors     = colorspace_num_colors(p->colorspace);
    p->bits_per_color = p->bits_per_pixel / p->num_colors;
    p->fill_byte      = colorspace_fill(p->colorspace);
    p->bpp_bytes      = (uint8_t)((p->bits_per_pixel + 7) / 8);
    if (p->bpp_bytes == 0) p->bpp_bytes = 1;

    // URF has no sub-byte depths, so this cannot lose a partial byte.
    p->bytes_per_line = (size_t)p->width * p->bits_per_pixel / 8;

    if (p->bytes_per_line > u->line_cap) {
        uint8_t *nl = realloc(u->line, p->bytes_per_line);
        if (!nl) return URF_ERR_IO;
        u->line     = nl;
        u->line_cap = p->bytes_per_line;
    }

    u->repeat_left  = 0;
    u->rows_emitted = 0;
    u->pages_seen++;
    return URF_OK;
}

int urf_next_row(urf_t *u, const uint8_t **row)
{
    (void)u; (void)row;
    return URF_ERR_UNSUPPORTED;   // Task 3
}

void urf_free(urf_t *u)
{
    free(u->line);
    u->line     = NULL;
    u->line_cap = 0;
}
