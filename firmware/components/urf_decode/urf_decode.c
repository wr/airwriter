// SPDX-License-Identifier: GPL-3.0-or-later
#include "urf_decode.h"
#include "bigmem.h"
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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
        // first page header. PWG support is deferred entirely (no 1796-byte
        // header parser exists yet); reject it here rather than silently
        // mis-parsing.
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

    // A malformed width must not wrap the multiply on a 32-bit target,
    // where size_t is 32 bits. 1152 dots is the printer's real limit;
    // 65535 is a generous ceiling that still cannot overflow.
    if (p->width > 65535 || p->height > 65535) return URF_ERR_UNSUPPORTED;

    p->num_colors     = colorspace_num_colors(p->colorspace);
    p->bits_per_color = p->bits_per_pixel / p->num_colors;
    p->fill_byte      = colorspace_fill(p->colorspace);
    p->bpp_bytes      = (uint8_t)((p->bits_per_pixel + 7) / 8);
    if (p->bpp_bytes == 0) p->bpp_bytes = 1;

    // URF has no sub-byte depths, so this cannot lose a partial byte.
    p->bytes_per_line = (size_t)p->width * p->bits_per_pixel / 8;

    // Veto an oversized format before allocating for it. Without this, a
    // header claiming e.g. 65535 wide at high bpp forces a multi-megabyte
    // allocation -- fine on a host, fatal on a 512 KB target.
    if (p->bytes_per_line > URF_MAX_BYTES_PER_LINE) return URF_ERR_UNSUPPORTED;

    if (p->bytes_per_line > u->line_cap) {
        uint8_t *nl = big_realloc(u->line, p->bytes_per_line);
        if (!nl) return URF_ERR_ALLOC;
        u->line     = nl;
        u->line_cap = p->bytes_per_line;
    }

    u->repeat_left  = 0;
    u->rows_emitted = 0;
    u->pages_seen++;
    return URF_OK;
}

// Decode one line into u->line. Sets *repeat to the number of times the
// caller should deliver it (the leading byte plus one).
//
// Ported from _cupsRasterReadPixels() in cups/raster-stream.c and cross-read
// against PWG 5102.4-2012 section 4.4. Note that this is PackBits with the
// ranges swapped: low control bytes repeat, high control bytes are literal.
static int decode_line(urf_t *u, unsigned *repeat)
{
    const urf_page_t *p = &u->page;
    uint8_t  ctl;
    uint8_t *dst   = u->line;
    ssize_t  bytes = (ssize_t)p->bytes_per_line;

    if (!u->read(u->ctx, &ctl, 1)) return URF_ERR_IO;
    *repeat = (unsigned)ctl + 1;          // 0 means once, never zero times

    while (bytes > 0) {
        if (!u->read(u->ctx, &ctl, 1)) return URF_ERR_IO;

        if (ctl == 0x80) {
            // Undefined in PWG 5102.4; both CUPS and urftopdf treat it as
            // clear-to-end-of-line. The fill depends on the colour space.
            memset(dst, p->fill_byte, (size_t)bytes);
            dst  += bytes;
            bytes = 0;
        } else if (ctl & 0x80) {
            // Literal: 257 - ctl colour values, so 2..128. A literal run of
            // one is not encodable.
            size_t n = (size_t)(257 - ctl) * p->bpp_bytes;
            // A run never legitimately crosses a line boundary. Clamping here
            // would leave the run's remaining bytes unread and desynchronise
            // every later line, so treat it as corrupt input instead.
            if (n > (size_t)bytes) return URF_ERR_MALFORMED;
            if (!u->read(u->ctx, dst, n)) return URF_ERR_IO;
            dst   += n;
            bytes -= (ssize_t)n;
        } else {
            // Repeat: ctl + 1 copies of the next colour value.
            size_t n = ((size_t)ctl + 1) * p->bpp_bytes;
            if (n > (size_t)bytes) return URF_ERR_MALFORMED;
            bytes -= (ssize_t)n;
            if (!u->read(u->ctx, dst, p->bpp_bytes)) return URF_ERR_IO;
            dst += p->bpp_bytes;
            n   -= p->bpp_bytes;
            while (n > 0) {
                memcpy(dst, dst - p->bpp_bytes, p->bpp_bytes);
                dst += p->bpp_bytes;
                n   -= p->bpp_bytes;
            }
        }
    }

    return URF_OK;
}

int urf_next_row(urf_t *u, const uint8_t **row)
{
    // Bound by the declared height. The line-repeat byte lets a file claim
    // more rows than the header says; CUPS bounds this and urftopdf does
    // not, which is how urftopdf writes past the end of a page buffer.
    if (u->rows_emitted >= u->page.height) return URF_END;

    if (u->repeat_left == 0) {
        int rc = decode_line(u, &u->repeat_left);
        if (rc != URF_OK) return rc;
    }

    u->repeat_left--;
    u->rows_emitted++;
    *row = u->line;
    return URF_OK;
}

void urf_free(urf_t *u)
{
    free(u->line);
    u->line     = NULL;
    u->line_cap = 0;
}
