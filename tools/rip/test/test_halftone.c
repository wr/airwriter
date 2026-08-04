// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "halftone.h"

int main(void)
{
    /* -- threshold: < 128 is ink, >= 128 is paper -- */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_THRESHOLD, 8), 0);
        const uint8_t gray[8] = { 0, 127, 128, 255, 0, 0, 255, 255 };
        uint8_t bits[1] = { 0 };
        ht_row(&h, gray, bits);
        /* ink at x = 0,1,4,5 -> bits 7,6,3,2 -> 0b11001100 */
        CHECK_EQ_INT(bits[0], 0xCC);
        ht_free(&h);
    }

    /* -- solid black is all ink, solid white is none, for every algorithm -- */
    {
        const ht_algo_t algos[] = { HT_THRESHOLD, HT_ATKINSON, HT_FLOYD, HT_BAYER };
        for (unsigned i = 0; i < sizeof algos / sizeof algos[0]; i++) {
            ht_t h;
            CHECK_EQ_INT(ht_init(&h, algos[i], 16), 0);
            uint8_t gray[16], bits[2];

            memset(gray, 0x00, sizeof gray);
            for (int y = 0; y < 4; y++) {
                memset(bits, 0, sizeof bits);
                ht_row(&h, gray, bits);
                CHECK_EQ_INT(bits[0], 0xFF);
                CHECK_EQ_INT(bits[1], 0xFF);
            }
            ht_free(&h);

            CHECK_EQ_INT(ht_init(&h, algos[i], 16), 0);
            memset(gray, 0xFF, sizeof gray);
            for (int y = 0; y < 4; y++) {
                memset(bits, 0xFF, sizeof bits);
                ht_row(&h, gray, bits);
                CHECK_EQ_INT(bits[0], 0x00);
                CHECK_EQ_INT(bits[1], 0x00);
            }
            ht_free(&h);
        }
    }

    /* -- Atkinson pushes exactly err/8 downward --
     *
     * Width 1 on purpose. A wider row would have the same pixel's own
     * horizontal spread land back on later pixels of that row and feed into
     * their errors, so the expected values stop being hand-checkable. With
     * one column, only the two downward taps survive.
     *
     * v = 128, so adj = 128, which is >= 128 and prints white (out = 255).
     * error = 128 - 255 = -127, and C truncates toward zero: -127 / 8 = -15.
     * The taps at x+1, x+2 and x-1 are all clipped away, leaving r1[0] and
     * r2[0].
     *
     * ht_peek_err(h, 0, x) reads the row about to be processed next.
     */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_ATKINSON, 1), 0);
        const uint8_t row0[1] = { 128 };
        uint8_t bits[1] = { 0xFF };
        ht_row(&h, row0, bits);
        CHECK_EQ_INT(bits[0], 0x00);              /* nothing fired */
        CHECK_EQ_INT(ht_peek_err(&h, 0, 0), -15); /* next row */
        CHECK_EQ_INT(ht_peek_err(&h, 1, 0), -15); /* the row after that */
        CHECK_EQ_INT(ht_peek_err(&h, 2, 0), 0);   /* slot just cleared */
        ht_free(&h);
    }

    /* -- algorithm names round-trip -- */
    {
        ht_algo_t a;
        CHECK_EQ_INT(ht_algo_from_name("atkinson", &a), 0);
        CHECK_EQ_INT(a, HT_ATKINSON);
        CHECK_EQ_INT(ht_algo_from_name("bayer", &a), 0);
        CHECK_EQ_INT(a, HT_BAYER);
        CHECK_EQ_INT(ht_algo_from_name("nonsense", &a), -1);
    }

    TEST_END;
}
