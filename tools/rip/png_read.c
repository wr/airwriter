// SPDX-License-Identifier: GPL-3.0-or-later
#include "png_read.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static const uint8_t k_sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

int png_is_png(const uint8_t *b, size_t n)
{
    return n >= sizeof k_sig && !memcmp(b, k_sig, sizeof k_sig);
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

typedef struct {
    uint32_t w, h;
    int      depth, ctype, channels;
    size_t   rowbytes, bpp;                 // bytes in a row (less its filter byte); bytes a pixel, 1 at least
    uint8_t  pal[256][3], alpha[256];
    int      npal;
    uint32_t dpi;
} png_t;

// One sample of a pixel at `depth` bits, from the unfiltered row: x counts samples across.
static unsigned sample(const uint8_t *row, int depth, size_t x)
{
    switch (depth) {
    case 8:  return row[x];
    case 16: return ((unsigned)row[2 * x] << 8) | row[2 * x + 1];
    default: {
        size_t bit = x * (size_t)depth;
        return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    }
    }
}

// The sample as 0..255.
static uint8_t scale8(unsigned v, int depth)
{
    if (depth == 8) return (uint8_t)v;
    unsigned max = (1u << depth) - 1;
    return (uint8_t)((v * 255u + max / 2) / max);
}

static uint8_t over_white(unsigned c, unsigned a)
{
    return (uint8_t)((c * a + 255u * (255u - a) + 127u) / 255u);
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

// Undoes the row filters in place. Rows are rowbytes + 1 long: the filter type, then the row.
static int unfilter(uint8_t *raw, const png_t *g)
{
    size_t stride = g->rowbytes + 1;
    for (uint32_t y = 0; y < g->h; y++) {
        uint8_t *row = raw + y * stride + 1, *prior = y ? row - stride : NULL;
        switch (row[-1]) {
        case 0: break;
        case 1: for (size_t i = g->bpp; i < g->rowbytes; i++) row[i] = (uint8_t)(row[i] + row[i - g->bpp]); break;
        case 2: if (prior) for (size_t i = 0; i < g->rowbytes; i++) row[i] = (uint8_t)(row[i] + prior[i]); break;
        case 3:
            for (size_t i = 0; i < g->rowbytes; i++) {
                int a = i >= g->bpp ? row[i - g->bpp] : 0, b = prior ? prior[i] : 0;
                row[i] = (uint8_t)(row[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < g->rowbytes; i++) {
                int a = i >= g->bpp ? row[i - g->bpp] : 0, b = prior ? prior[i] : 0;
                int c = i >= g->bpp && prior ? prior[i - g->bpp] : 0;
                row[i] = (uint8_t)(row[i] + paeth(a, b, c));
            }
            break;
        default: return -1;
        }
    }
    return 0;
}

// The page's rows as the URF coder wants them: `out` channels of 8 bits a pixel.
static void to_rgb_or_gray(const uint8_t *row, const png_t *g, int out_ch, uint8_t *dst)
{
    for (uint32_t x = 0; x < g->w; x++) {
        unsigned r, gr, b, a = 255;
        switch (g->ctype) {
        case 0:  r = gr = b = scale8(sample(row, g->depth, x), g->depth); break;
        case 4:  r = gr = b = scale8(sample(row, g->depth, 2 * x), g->depth);
                 a = scale8(sample(row, g->depth, 2 * x + 1), g->depth); break;
        case 2:  r  = scale8(sample(row, g->depth, 3 * x), g->depth);
                 gr = scale8(sample(row, g->depth, 3 * x + 1), g->depth);
                 b  = scale8(sample(row, g->depth, 3 * x + 2), g->depth); break;
        case 6:  r  = scale8(sample(row, g->depth, 4 * x), g->depth);
                 gr = scale8(sample(row, g->depth, 4 * x + 1), g->depth);
                 b  = scale8(sample(row, g->depth, 4 * x + 2), g->depth);
                 a  = scale8(sample(row, g->depth, 4 * x + 3), g->depth); break;
        default: {                                    // 3, a palette; an index past it is black
            unsigned i = sample(row, g->depth, x);
            if (i >= (unsigned)g->npal) i = 0;
            r = g->pal[i][0]; gr = g->pal[i][1]; b = g->pal[i][2];
            a = g->alpha[i];
        }
        }
        if (a != 255) { r = over_white(r, a); gr = over_white(gr, a); b = over_white(b, a); }
        if (out_ch == 1) { *dst++ = (uint8_t)r; }
        else { *dst++ = (uint8_t)r; *dst++ = (uint8_t)gr; *dst++ = (uint8_t)b; }
    }
}

// One URF line (without its repeat byte) of `n` pixels of `ch` bytes: a repeat code for a run
// of three or more equal pixels (up to 128), a literal for the rest (2 to 128; one pixel is a
// repeat of one). Returns the bytes written; `out` has room for n * ch + n / 64 + 2.
static size_t urf_line(const uint8_t *px, uint32_t n, int ch, uint8_t *out)
{
    uint8_t *o = out;
    uint32_t x = 0;
    while (x < n) {
        uint32_t run = 1;
        while (x + run < n && run < 128 && !memcmp(px + (size_t)x * ch, px + (size_t)(x + run) * ch, (size_t)ch)) run++;
        if (run >= 3 || x + run == n) {
            *o++ = (uint8_t)(run - 1);
            memcpy(o, px + (size_t)x * ch, (size_t)ch);
            o += ch;
            x += run;
            continue;
        }
        uint32_t start = x;                           // a literal, up to where a run of 3 begins
        while (x < n && x - start < 128) {
            if (x + 2 < n && !memcmp(px + (size_t)x * ch, px + (size_t)(x + 1) * ch, (size_t)ch) &&
                !memcmp(px + (size_t)x * ch, px + (size_t)(x + 2) * ch, (size_t)ch)) break;
            x++;
        }
        uint32_t k = x - start;
        *o++ = k == 1 ? 0 : (uint8_t)(257 - k);
        memcpy(o, px + (size_t)start * ch, (size_t)k * ch);
        o += (size_t)k * ch;
    }
    return (size_t)(o - out);
}

#define FAIL(msg) do { *why = (msg); goto done; } while (0)

int png_to_urf(const uint8_t *png, size_t n, uint32_t default_dpi, uint8_t **urf, size_t *urf_n,
               const char **why)
{
    png_t g;
    memset(&g, 0, sizeof g);
    g.dpi = default_dpi;
    uint8_t *idat = NULL, *raw = NULL, *line = NULL, *prev = NULL, *res = NULL;
    size_t idat_n = 0, idat_cap = 0;
    int have_ihdr = 0, ended = 0, rc = -1;
    *urf = NULL;
    *why = "damaged PNG";

    if (!png_is_png(png, n)) FAIL("not a PNG");
    size_t pos = sizeof k_sig;
    while (pos + 12 <= n && !ended) {
        uint32_t len = be32(png + pos);
        const uint8_t *type = png + pos + 4, *data = png + pos + 8;
        if (len > n - pos - 12) FAIL("damaged PNG (cut short)");
        uLong want = be32(data + len), have = crc32(crc32(0, NULL, 0), type, len + 4);
        if (want != have) FAIL("damaged PNG (a chunk fails its check)");
        if (!memcmp(type, "IHDR", 4)) {
            if (len != 13) FAIL("damaged PNG (header)");
            g.w = be32(data); g.h = be32(data + 4);
            g.depth = data[8]; g.ctype = data[9];
            if (data[10] || data[11]) FAIL("PNG with an unknown compression or filter method");
            if (data[12]) FAIL("interlaced PNG: save it again without interlacing");
            int ok = g.ctype == 0 ? (g.depth == 1 || g.depth == 2 || g.depth == 4 || g.depth == 8 || g.depth == 16)
                   : g.ctype == 3 ? (g.depth == 1 || g.depth == 2 || g.depth == 4 || g.depth == 8)
                   : (g.ctype == 2 || g.ctype == 4 || g.ctype == 6) ? (g.depth == 8 || g.depth == 16) : 0;
            if (!ok) FAIL("PNG with a colour type or bit depth that does not exist");
            if (!g.w || !g.h || g.w > 65535 || g.h > 65535) FAIL("PNG too big (65535 pixels a side at most)");
            g.channels = g.ctype == 2 ? 3 : g.ctype == 4 ? 2 : g.ctype == 6 ? 4 : 1;
            g.rowbytes = ((size_t)g.w * (size_t)g.channels * (size_t)g.depth + 7) / 8;
            g.bpp = ((size_t)g.channels * (size_t)g.depth + 7) / 8;
            have_ihdr = 1;
            memset(g.alpha, 255, sizeof g.alpha);
        } else if (!have_ihdr) {
            FAIL("damaged PNG (no header first)");
        } else if (!memcmp(type, "PLTE", 4)) {
            if (len % 3 || len > 768 || !len) FAIL("damaged PNG (palette)");
            g.npal = (int)(len / 3);
            memcpy(g.pal, data, len);
        } else if (!memcmp(type, "tRNS", 4)) {
            if (g.ctype == 3) memcpy(g.alpha, data, len < 256 ? len : 256);
        } else if (!memcmp(type, "pHYs", 4)) {
            if (len == 9 && data[8] == 1 && be32(data) > 0) {
                uint32_t dpi = (uint32_t)(((uint64_t)be32(data) * 254u + 5000u) / 10000u);   // pixels a metre to dots an inch
                if (dpi) g.dpi = dpi;
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_n + len > idat_cap) {
                size_t cap = (idat_n + len) * 2;
                uint8_t *nb = realloc(idat, cap);
                if (!nb) FAIL("out of memory");
                idat = nb;
                idat_cap = cap;
            }
            memcpy(idat + idat_n, data, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            ended = 1;
        }
        pos += 12 + len;
    }
    if (!have_ihdr || !ended || !idat_n) FAIL("damaged PNG (cut short)");
    if (g.ctype == 3 && !g.npal) FAIL("damaged PNG (no palette)");

    size_t stride = g.rowbytes + 1, want_n = stride * g.h;
    if (g.h && want_n / g.h != stride) FAIL("PNG too big");
    raw = malloc(want_n);
    if (!raw) FAIL("out of memory");
    uLongf got = (uLongf)want_n;
    if (uncompress(raw, &got, idat, (uLong)idat_n) != Z_OK || got != want_n) FAIL("damaged PNG (its picture does not unpack)");
    if (unfilter(raw, &g)) FAIL("damaged PNG (a row filter that does not exist)");

    int gray = g.ctype == 0 || g.ctype == 4;
    if (g.ctype == 3) {
        gray = 1;
        for (int i = 0; i < g.npal; i++)
            if (g.pal[i][0] != g.pal[i][1] || g.pal[i][1] != g.pal[i][2]) { gray = 0; break; }
    }
    int ch = gray ? 1 : 3;
    size_t rb = (size_t)g.w * (size_t)ch;
    // A line codes to at most its pixels, one control byte in 128 and the repeat byte; the
    // header is 12 + 32.
    size_t line_max = 1 + rb + rb / 64 + 4;
    res = malloc(44 + line_max * g.h);
    line = malloc(rb);
    prev = malloc(rb);
    if (!res || !line || !prev) FAIL("out of memory");
    memcpy(res, "UNIRAST\0", 8);
    memset(res + 8, 0, 36);
    res[11] = 1;                                      // one page
    uint8_t *hdr = res + 12;
    hdr[0] = (uint8_t)(8 * ch);
    hdr[1] = gray ? 0 : 1;                            // sGray, sRGB
    hdr[12] = (uint8_t)(g.w >> 24); hdr[13] = (uint8_t)(g.w >> 16); hdr[14] = (uint8_t)(g.w >> 8); hdr[15] = (uint8_t)g.w;
    hdr[16] = (uint8_t)(g.h >> 24); hdr[17] = (uint8_t)(g.h >> 16); hdr[18] = (uint8_t)(g.h >> 8); hdr[19] = (uint8_t)g.h;
    hdr[20] = (uint8_t)(g.dpi >> 24); hdr[21] = (uint8_t)(g.dpi >> 16); hdr[22] = (uint8_t)(g.dpi >> 8); hdr[23] = (uint8_t)g.dpi;
    uint8_t *o = res + 44;
    uint32_t y = 0;
    while (y < g.h) {
        to_rgb_or_gray(raw + y * stride + 1, &g, ch, line);
        // The same row again is a repeat of the line (up to 256 times).
        uint32_t same = 1;
        while (y + same < g.h && same < 256) {
            to_rgb_or_gray(raw + (y + same) * stride + 1, &g, ch, prev);
            if (memcmp(line, prev, rb)) break;
            same++;
        }
        *o++ = (uint8_t)(same - 1);
        o += urf_line(line, g.w, ch, o);
        y += same;
    }
    *urf = res;
    *urf_n = (size_t)(o - res);
    res = NULL;
    rc = 0;
done:
    free(idat); free(raw); free(line); free(prev); free(res);
    return rc;
}
