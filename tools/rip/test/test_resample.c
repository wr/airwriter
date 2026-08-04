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

    TEST_END;
}
