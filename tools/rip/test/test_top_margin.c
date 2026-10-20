// SPDX-License-Identifier: GPL-3.0-or-later
//
// The top margin setting through ripjob: blank rows added ahead of a page, or
// the page's first rows dropped, at the printer's 1/72 in row.
#include "test_util.h"
#include "ripjob.h"
#include <stdlib.h>

#define W   16
#define H   48              /* source rows at 144 dpi: 24 printer rows, 3 strips */
#define RES 144

/* A one-page URF, every row sent as literal runs. px is W*H*bpp bytes. */
static size_t make_urf(uint8_t *out, uint8_t bpp_bits, uint8_t colorspace, const uint8_t *px)
{
    size_t n = 0, bpp = bpp_bits / 8;
    memcpy(out, "UNIRAST\0", 8); n = 8;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = bpp_bits;
    hdr[1] = colorspace;
    hdr[15] = W;  hdr[19] = H;  hdr[23] = RES;
    memcpy(out + n, hdr, 32); n += 32;
    for (int y = 0; y < H; y++) {
        out[n++] = 0;
        out[n++] = (uint8_t)(257 - W);
        memcpy(out + n, px + (size_t)y * W * bpp, W * bpp);
        n += W * bpp;
    }
    return n;
}

static uint8_t stream[8192];
static size_t  stream_n;
static void collect(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (stream_n + n <= sizeof stream) memcpy(stream + stream_n, b, n);
    stream_n += n;
}

static int rip(const uint8_t *urf, size_t len, int colour, int top)
{
    memsrc_t m = { urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    ripjob_cfg_t cfg = { .out = collect, .dither = HT_THRESHOLD, .colour = colour,
                         .top_margin = top };
    stream_n = 0;
    int rc = ripjob_run(&u, &cfg, NULL);
    urf_free(&u);
    return rc;
}

/* The printer's row of the page's first dot, from the stream: LFs of 16/144
 * in are 8 rows, a strip's bit r is its row r. -1 for a page with no dots. */
static int first_dot_row(void)
{
    int y = 0, first = -1;
    for (size_t i = 0; i < stream_n;) {
        uint8_t c = stream[i++];
        if (c == '\n') { y += 8; continue; }
        if (c != 0x1B) continue;
        uint8_t k = stream[i++];
        if (k == 'T' ) { i += 2; continue; }
        if (k == 'K' ) { i += 1; continue; }
        if (k != 'G') continue;
        int n = (stream[i] - '0') * 1000 + (stream[i + 1] - '0') * 100 +
                (stream[i + 2] - '0') * 10 + (stream[i + 3] - '0');
        i += 4;
        for (int x = 0; x < n; x++)
            for (int r = 0; r < 8; r++)
                if (stream[i + x] >> r & 1 && (first < 0 || y + r < first)) first = y + r;
        i += (size_t)n;
    }
    return first;
}

int main(void)
{
    /* White, but printer rows 8-15 (source rows 16-31) black. */
    static uint8_t gray[W * H], rgb[W * H * 3], urf_g[8192], urf_c[16384];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t v = (y >= 16 && y < 32) ? 0 : 255;
            gray[y * W + x] = v;
            for (int c = 0; c < 3; c++) rgb[(y * W + x) * 3 + c] = v;
        }
    size_t ng = make_urf(urf_g, 8, 0, gray), nc = make_urf(urf_c, 24, 1, rgb);

    /* In 1/144 in; a row is 2. Odd values round toward zero. */
    static const struct { int top, row; } cases[] = {
        { 0, 8 }, { 16, 16 }, { -16, 0 }, { 9, 12 }, { -9, 4 }, { 144, 80 }, { -12, 2 },
    };
    for (int colour = 0; colour < 2; colour++) {
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            CHECK_EQ_INT(colour ? rip(urf_c, nc, 1, cases[k].top) : rip(urf_g, ng, 0, cases[k].top), 0);
            if (first_dot_row() != cases[k].row)
                fprintf(stderr, "  colour %d, top %d\n", colour, cases[k].top);
            CHECK_EQ_INT(first_dot_row(), cases[k].row);
            CHECK(stream_n && stream[stream_n - 1] == 0x0C);
        }
        /* Dropped past its content, a page is its form feed. */
        CHECK_EQ_INT(colour ? rip(urf_c, nc, 1, -48) : rip(urf_g, ng, 0, -48), 0);
        CHECK_EQ_INT(first_dot_row(), -1);
    }

    /* No margin is the stream as it always was. */
    static uint8_t plain[8192];
    rip(urf_g, ng, 0, 0);
    size_t pn = stream_n;
    memcpy(plain, stream, pn);
    memsrc_t m = { urf_g, ng, 0 };
    urf_t u;
    urf_open(&u, memsrc_read, &m);
    ripjob_cfg_t cfg = { .out = collect, .dither = HT_THRESHOLD };
    stream_n = 0;
    ripjob_run(&u, &cfg, NULL);
    urf_free(&u);
    CHECK_EQ_INT(stream_n, pn);
    CHECK_EQ_MEM(stream, plain, pn);

    TEST_END;
}
