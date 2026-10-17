// SPDX-License-Identifier: GPL-3.0-or-later
//
// RGB pages through ripjob (W-1064): four inks with the colour ribbon in,
// gray otherwise, and gray pages untouched either way.
#include "test_util.h"
#include "ripjob.h"
#include <stdlib.h>

#define W   16
#define H   16
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
        out[n++] = 0;                         /* line repeat: once */
        out[n++] = (uint8_t)(257 - W);        /* W literal pixels */
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

static int ink_rows;
static void on_ink_row(void *ctx, const uint8_t *const bits[IW_INKS], uint32_t w, uint32_t y)
{
    (void)ctx; (void)bits; (void)w; (void)y;
    ink_rows++;
}

/* Rips a URF; the stream lands in stream[]. Returns ripjob_run's result. */
static int rip(const uint8_t *urf, size_t len, int colour)
{
    memsrc_t m = { urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    ripjob_cfg_t cfg = { .out = collect, .dither = HT_ATKINSON, .colour = colour,
                         .ink_row = on_ink_row };
    stream_n = 0;
    ink_rows = 0;
    int rc = ripjob_run(&u, &cfg, NULL);
    urf_free(&u);
    return rc;
}

static int has(const char *needle, size_t k)
{
    for (size_t i = 0; i + k <= stream_n; i++)
        if (!memcmp(stream + i, needle, k)) return 1;
    return 0;
}

int main(void)
{
    static uint8_t gray[W * H], rgb[W * H * 3], red[W * H * 3];
    static uint8_t urf_a[4096], urf_b[8192];
    static uint8_t want[8192];
    for (int i = 0; i < W * H; i++) {
        gray[i] = (uint8_t)((i * 37) & 0xFF);
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = gray[i];
        red[i * 3] = 255; red[i * 3 + 1] = 0; red[i * 3 + 2] = 0;
    }

    /* rgb_without_colour_prints_gray: a black ribbon prints an RGB page as
     * the same levels sent as W8 would print */
    {
        size_t na = make_urf(urf_a, 8, 0, gray);
        CHECK_EQ_INT(rip(urf_a, na, 0), 0);
        size_t nw = stream_n;
        memcpy(want, stream, nw);

        size_t nb = make_urf(urf_b, 24, 1, rgb);
        CHECK_EQ_INT(rip(urf_b, nb, 0), 0);
        CHECK_EQ_INT(stream_n, nw);
        CHECK_EQ_MEM(stream, want, nw);
        CHECK(!has("\x1bK", 2));
        CHECK_EQ_INT(ink_rows, 0);

        /* gray_page_ignores_colour: W8 with the colour ribbon in is unchanged */
        CHECK_EQ_INT(rip(urf_a, na, 1), 0);
        CHECK_EQ_INT(stream_n, nw);
        CHECK_EQ_MEM(stream, want, nw);
    }

    /* colour_page_selects_inks: red is magenta over yellow, nothing else */
    {
        size_t nb = make_urf(urf_b, 24, 1, red);
        CHECK_EQ_INT(rip(urf_b, nb, 1), 0);
        CHECK(has("\x1bK1", 3));
        CHECK(has("\x1bK2", 3));
        CHECK(!has("\x1bK3", 3));
        CHECK_EQ_INT(stream[stream_n - 1], 0x0C);
        CHECK(has("\x1bK0\x0c", 4));          /* ends on black */
        CHECK_EQ_INT(ink_rows, H * 72 / RES);
    }

    /* cielab_refused: colorspace 2 is three colours too, but not RGB */
    {
        size_t nb = make_urf(urf_b, 24, 2, rgb);
        CHECK(rip(urf_b, nb, 1) < 0);
        CHECK(rip(urf_b, nb, 0) < 0);
    }

    TEST_END;
}
