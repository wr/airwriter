// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1178: crisp text and lines on an ImageWriter's colour page. The gray decides, and only gray
// pixels change: a gray hairline prints as an unbroken black line, a blue line as it did.
#include "test_util.h"
#include "ripjob.h"
#include <stdlib.h>

#define W   64
#define H   48                              // 144 dpi source rows: 24 of the ImageWriter's
#define RES 144

static size_t make_urf(uint8_t *out, const uint8_t *px)
{
    size_t n = 0;
    memcpy(out, "UNIRAST\0", 8); n = 8;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = 24;                            // sRGB
    hdr[1] = 1;
    hdr[15] = W;  hdr[19] = H;  hdr[23] = RES;
    memcpy(out + n, hdr, 32); n += 32;
    for (int y = 0; y < H; y++) {
        const uint8_t *row = px + (size_t)y * W * 3;
        out[n++] = 0;                       // the row once
        for (int x = 0; x < W; x += 32) {   // literal runs of 32
            out[n++] = (uint8_t)(257 - 32);
            memcpy(out + n, row + x * 3, 32 * 3);
            n += 32 * 3;
        }
    }
    return n;
}

static void nop(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; (void)n; }

static uint8_t g_ink[IW_INKS][H / 2][W];
static void on_ink(void *ctx, const uint8_t *const bits[IW_INKS], uint32_t w, uint32_t y)
{
    (void)ctx;
    if (y >= H / 2) return;
    for (int k = 0; k < IW_INKS; k++)
        for (uint32_t x = 0; x < w && x < W; x++) g_ink[k][y][x] = bits[k][x >> 3] >> (7 - (x & 7)) & 1;
}

static int rip(const uint8_t *urf, size_t len, int lineart)
{
    memsrc_t m = { urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    ripjob_cfg_t cfg = { .out = nop, .dither = HT_ATKINSON, .colour = 1, .ink_row = on_ink,
                         .lineart = lineart, .lineart_light = 247 };
    memset(g_ink, 0, sizeof g_ink);
    int rc = ripjob_run(&u, &cfg, NULL);
    urf_free(&u);
    return rc;
}

// The review of W-1178: a colour page shorter than the text and lines' ten rows of delay
// pairs each row with its own colour row, not one before the ring. One black hairline at the
// bottom row of a page of each height, across several pages in one job.
static size_t make_short(uint8_t *out, int pages, int rows)
{
    size_t n = 8;
    memcpy(out, "UNIRAST\0", 8);
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = (uint8_t)pages;
    for (int p = 0; p < pages; p++) {
        uint8_t hdr[32] = { 0 };
        hdr[0] = 24; hdr[1] = 1;
        hdr[15] = W; hdr[19] = (uint8_t)(rows * 2); hdr[23] = RES;
        memcpy(out + n, hdr, 32); n += 32;
        for (int y = 0; y < rows * 2; y++) {
            uint8_t v = y >= rows * 2 - 2 ? 0 : 255;
            out[n++] = 0;
            out[n++] = (uint8_t)(W - 1);    // W pixels, repeated
            out[n++] = v; out[n++] = v; out[n++] = v;
        }
    }
    return n;
}

static int g_rows, g_bad;
static void on_short(void *ctx, const uint8_t *const bits[IW_INKS], uint32_t w, uint32_t y)
{
    const int rows = *(const int *)ctx;
    int want = (int)(y % (uint32_t)rows) == rows - 1;
    for (uint32_t x = 4; x + 4 < w && x < W; x++) {
        int k = bits[IW_BLACK][x >> 3] >> (7 - (x & 7)) & 1;
        int c = (bits[IW_CYAN][x >> 3] | bits[IW_MAGENTA][x >> 3] | bits[IW_YELLOW][x >> 3]) >> (7 - (x & 7)) & 1;
        if (k != want || c) g_bad++;
    }
    g_rows++;
}

static void test_short_pages(void)
{
    static uint8_t urf[3 * (44 + 2 * 12 * 5)];
    for (int rows = 1; rows <= 12; rows++) {
        size_t n = make_short(urf, 3, rows);
        memsrc_t m = { urf, n, 0 };
        urf_t u;
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &m), URF_OK);
        int ctx = rows;
        ripjob_cfg_t cfg = { .out = nop, .dither = HT_ATKINSON, .colour = 1, .ink_row = on_short,
                             .row_ctx = &ctx, .lineart = 1, .lineart_light = 247 };
        g_rows = g_bad = 0;
        CHECK_EQ_INT(ripjob_run(&u, &cfg, NULL), 0);
        urf_free(&u);
        CHECK_EQ_INT(g_rows, 3 * rows);
        CHECK_EQ_INT(g_bad, 0);
    }
}

int main(void)
{
    test_short_pages();
    static uint8_t px[W * H * 3], urf[W * H * 4 + 64];
    static uint8_t off[IW_INKS][H / 2][W];
    memset(px, 255, sizeof px);
    for (int x = 8; x < 56; x++)             // a 16% gray hairline, two source rows: one of the
        for (int y = 20; y < 22; y++)        // ImageWriter's, row 10
            px[(y * W + x) * 3 + 0] = px[(y * W + x) * 3 + 1] = px[(y * W + x) * 3 + 2] = 215;
    for (int y = 4; y < 44; y++)             // a blue line, two dots wide
        for (int x = 30; x < 32; x++) {
            if (y >= 20 && y < 22) continue;
            px[(y * W + x) * 3 + 0] = 0; px[(y * W + x) * 3 + 1] = 0; px[(y * W + x) * 3 + 2] = 200;
        }
    size_t n = make_urf(urf, px);

    CHECK_EQ_INT(rip(urf, n, 0), 0);
    memcpy(off, g_ink, sizeof off);
    int black_off = 0;
    for (int x = 10; x < 54; x++) black_off += g_ink[IW_BLACK][10][x];
    CHECK(black_off < 44);                   // Atkinson drops most of it

    CHECK_EQ_INT(rip(urf, n, 1), 0);
    for (int x = 10; x < 54; x++) {
        if (x >= 29 && x <= 32) continue;    // where the blue line crosses
        CHECK_EQ_INT(g_ink[IW_BLACK][10][x], 1);
        CHECK_EQ_INT(g_ink[IW_CYAN][10][x] | g_ink[IW_MAGENTA][10][x] | g_ink[IW_YELLOW][10][x], 0);
    }
    // The blue line's inks are the same with and without: it isn't gray.
    for (int y = 0; y < H / 2; y++) {
        if (y >= 9 && y <= 11) continue;
        for (int x = 28; x < 34; x++)
            for (int k = 0; k < IW_INKS; k++) CHECK_EQ_INT(g_ink[k][y][x], off[k][y][x]);
    }
    TEST_END;
}
