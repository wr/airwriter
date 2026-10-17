// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "resample.h"

#define MAXROWS 16
#define MAXW    16

static uint8_t got[MAXROWS][MAXW];
static uint32_t got_n;

static void collect(void *ctx, const uint8_t *row, uint32_t w)
{
    (void)ctx;
    if (got_n < MAXROWS) memcpy(got[got_n], row, w);
    got_n++;
}

/* Rows from a multi-channel resampler, and from one per channel. */
static uint8_t gotn[MAXROWS][3 * MAXW];
static uint32_t gotn_n;
static void collect_n(void *ctx, const uint8_t *row, uint32_t w)
{
    (void)ctx;
    if (gotn_n < MAXROWS) memcpy(gotn[gotn_n], row, 3 * w);
    gotn_n++;
}

/* Resamples a w x h 3-channel source to dw x dh both ways and checks every
 * channel of every row agrees with a 1-channel resample of that plane. */
static void check_three_vs_singles(const uint8_t *src, uint32_t w, uint32_t h,
                                   uint32_t dw, uint32_t dh)
{
    rs_t r;
    gotn_n = 0;
    CHECK_EQ_INT(rs_init_n(&r, w, h, dw, dh, 3, collect_n, NULL), 0);
    for (uint32_t y = 0; y < h; y++) CHECK_EQ_INT(rs_push(&r, src + y * w * 3), 0);
    rs_free(&r);
    CHECK_EQ_INT(gotn_n, dh);

    for (uint32_t c = 0; c < 3; c++) {
        uint8_t plane[MAXW];
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, w, h, dw, dh, collect, NULL), 0);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) plane[x] = src[(y * w + x) * 3 + c];
            rs_push(&r, plane);
        }
        rs_free(&r);
        CHECK_EQ_INT(got_n, dh);
        for (uint32_t y = 0; y < dh; y++)
            for (uint32_t x = 0; x < dw; x++)
                CHECK_EQ_INT(gotn[y][x * 3 + c], got[y][x]);
    }
}

int main(void)
{
    /* -- 2:1 downscale in both axes, box averaged -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 4, 4, 2, 2, collect, NULL), 0);

        const uint8_t a[4] = {   0,  20,  40,  60 };
        const uint8_t b[4] = { 100, 120, 140, 160 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);

        CHECK_EQ_INT(got_n, 2);
        /* first dst row averages a and b over 2x2 blocks:
         * (0+20+100+120)/4 = 60,  (40+60+140+160)/4 = 100 */
        CHECK_EQ_INT(got[0][0], 60);
        CHECK_EQ_INT(got[0][1], 100);
        CHECK_EQ_INT(got[1][0], 60);
        CHECK_EQ_INT(got[1][1], 100);
        rs_free(&r);
    }

    /* -- 1:1 passes rows through untouched -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 3, 2, 3, 2, collect, NULL), 0);
        const uint8_t a[3] = { 1, 2, 3 };
        const uint8_t b[3] = { 4, 5, 6 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(got_n, 2);
        CHECK_EQ_MEM(got[0], a, 3);
        CHECK_EQ_MEM(got[1], b, 3);
        rs_free(&r);
    }

    /* -- upscale replicates: 2 source rows to 4 destination rows -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 2, 2, 2, 4, collect, NULL), 0);
        const uint8_t a[2] = { 10, 20 };
        const uint8_t b[2] = { 30, 40 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(got_n, 4);
        CHECK_EQ_INT(got[0][0], 10);
        CHECK_EQ_INT(got[1][0], 10);
        CHECK_EQ_INT(got[2][0], 30);
        CHECK_EQ_INT(got[3][0], 30);
        rs_free(&r);
    }

    /* -- horizontal upscale: 2 wide becomes 4 wide, height unchanged -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 2, 2, 4, 2, collect, NULL), 0);
        /* each pixel replicates horizontally: source[0] → dest[0:2], source[1] → dest[2:4] */
        const uint8_t a[2] = { 50, 100 };
        const uint8_t b[2] = { 150, 200 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(got_n, 2);
        CHECK_EQ_INT(got[0][0], 50);
        CHECK_EQ_INT(got[0][1], 50);
        CHECK_EQ_INT(got[0][2], 100);
        CHECK_EQ_INT(got[0][3], 100);
        CHECK_EQ_INT(got[1][0], 150);
        CHECK_EQ_INT(got[1][1], 150);
        CHECK_EQ_INT(got[1][2], 200);
        CHECK_EQ_INT(got[1][3], 200);
        rs_free(&r);
    }

    /* -- mixed axes: width 2x (144 dpi), height 1/2 (72 dpi) -- */
    {
        rs_t r;
        got_n = 0;
        CHECK_EQ_INT(rs_init(&r, 4, 4, 8, 2, collect, NULL), 0);
        const uint8_t a[4] = { 10, 20, 30, 40 };
        const uint8_t b[4] = { 50, 60, 70, 80 };
        const uint8_t c[4] = { 90, 100, 110, 120 };
        const uint8_t d[4] = { 130, 140, 150, 160 };
        CHECK_EQ_INT(rs_push(&r, a), 0);
        CHECK_EQ_INT(rs_push(&r, b), 0);
        CHECK_EQ_INT(rs_push(&r, c), 0);
        CHECK_EQ_INT(rs_push(&r, d), 0);
        CHECK_EQ_INT(got_n, 2);
        /* first dst row: box average a,b over 2x2 blocks, then replicate horizontally
         * a,b block [0]: (10+50)/2 = 30, replicate → 30,30
         * a,b block [1]: (20+60)/2 = 40, replicate → 40,40
         * a,b block [2]: (30+70)/2 = 50, replicate → 50,50
         * a,b block [3]: (40+80)/2 = 60, replicate → 60,60 */
        CHECK_EQ_INT(got[0][0], 30);
        CHECK_EQ_INT(got[0][1], 30);
        CHECK_EQ_INT(got[0][2], 40);
        CHECK_EQ_INT(got[0][3], 40);
        CHECK_EQ_INT(got[0][4], 50);
        CHECK_EQ_INT(got[0][5], 50);
        CHECK_EQ_INT(got[0][6], 60);
        CHECK_EQ_INT(got[0][7], 60);
        /* second dst row: box average c,d over 2x2 blocks, then replicate horizontally
         * c,d block [0]: (90+130)/2 = 110, replicate → 110,110
         * c,d block [1]: (100+140)/2 = 120, replicate → 120,120
         * c,d block [2]: (110+150)/2 = 130, replicate → 130,130
         * c,d block [3]: (120+160)/2 = 140, replicate → 140,140 */
        CHECK_EQ_INT(got[1][0], 110);
        CHECK_EQ_INT(got[1][1], 110);
        CHECK_EQ_INT(got[1][2], 120);
        CHECK_EQ_INT(got[1][3], 120);
        CHECK_EQ_INT(got[1][4], 130);
        CHECK_EQ_INT(got[1][5], 130);
        CHECK_EQ_INT(got[1][6], 140);
        CHECK_EQ_INT(got[1][7], 140);
        rs_free(&r);
    }

    /* -- three_channels_match_three_singles: interleaved RGB, down and up -- */
    {
        uint8_t src[7 * 5 * 3];
        uint32_t v = 12345;
        for (size_t i = 0; i < sizeof src; i++) { v = v * 1103515245u + 12345u; src[i] = (uint8_t)(v >> 16); }
        check_three_vs_singles(src, 7, 5, 3, 2);
        check_three_vs_singles(src, 7, 5, 14, 10);
        check_three_vs_singles(src, 7, 5, 7, 5);
    }

    TEST_END;
}
