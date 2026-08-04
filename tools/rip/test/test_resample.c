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

    TEST_END;
}
