// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "halftone.h"
#include "apple_tables.h"

/* Row y of what Apple's tile should print at an amount of ink, as bits: a cell inks
 * where the amount is over its threshold, the threshold read mirrored about 254
 * when `turned`, the tile started px across and py down. Compared a row at a time:
 * the same check summed cell by cell in one loop counted 30 wrong at -O2 under Apple
 * clang 21 (-O0, -O1 and the sanitizers, 0), though no cell it looked at differed. */
static void apple_row(uint8_t *out, int w, int y, int px, int py, int turned, int amount)
{
    memset(out, 0, (size_t)(w + 7) / 8);
    for (int x = 0; x < w; x++) {
        int t = apple_screen16[((y + py) & 15) * 16 + ((x + px) & 15)];
        if (turned) t = 254 - t;
        if (amount > t) out[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}

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

    /* -- Floyd-Steinberg spreads error with kernel 7/16, 3/16, 5/16, 1/16 --
     *
     * Width 3, row { 255, 128, 255 }:
     *
     * x=0: raw 255, out 255, error 0 — spreads nothing.
     * x=1: raw 128, out 255, error -127. Taps:
     *      r0[2] += -127*7/16 = -55; r1[0] += -127*3/16 = -23;
     *      r1[1] += -127*5/16 = -39; r1[2] += -127*1/16 = -7.
     * x=2: raw = 255 + (-55) = 200, out 255, error -55. Taps:
     *      r1[1] += -55*3/16 = -10; r1[2] += -55*5/16 = -17.
     *      (7/16 and 1/16 taps fall outside the row.)
     *
     * After row: ht_peek_err(&h, 0, 0) = -23; [0, 1] = -49; [0, 2] = -24.
     */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_FLOYD, 3), 0);
        const uint8_t row0[3] = { 255, 128, 255 };
        uint8_t bits[1] = { 0 };
        ht_row(&h, row0, bits);
        CHECK_EQ_INT(ht_peek_err(&h, 0, 0), -23);
        CHECK_EQ_INT(ht_peek_err(&h, 0, 1), -49);
        CHECK_EQ_INT(ht_peek_err(&h, 0, 2), -24);
        ht_free(&h);
    }

    /* -- algorithm names round-trip -- */
    {
        ht_algo_t a;
        CHECK_EQ_INT(ht_algo_from_name("atkinson", &a), 0);
        CHECK_EQ_INT(a, HT_ATKINSON);
        CHECK_EQ_INT(ht_algo_from_name("bayer", &a), 0);
        CHECK_EQ_INT(a, HT_BAYER);
        CHECK_EQ_INT(ht_algo_from_name("bluenoise", &a), 0);
        CHECK_EQ_INT(a, HT_BLUENOISE);
        CHECK_EQ_INT(ht_algo_from_name("nonsense", &a), -1);
    }

    /* -- serpentine: the second row is scanned from the right --
     *
     * Floyd-Steinberg, width 3. Row 0 is all paper, so it carries nothing.
     * Row 1 is { 255, 128, 255 } scanned right to left: x=2 prints paper
     * with no error, x=1 is 128, prints paper, error -127, and its 7/16 tap
     * goes the way the scan goes, to x=0 (-55), so x=0 is 200 and still
     * paper. On a left-to-right row that -55 would have landed on x=2. The
     * next row's taps mirror too: 3/16 goes to x=2 (-23), 1/16 to x=0 (-7),
     * then x=0's own -55 spreads 3/16 to x=1 (-10) and 5/16 to x=0 (-17).
     */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_FLOYD, 3), 0);
        const uint8_t paper[3] = { 255, 255, 255 }, row1[3] = { 255, 128, 255 };
        uint8_t bits[1] = { 0 };
        ht_row(&h, paper, bits);
        ht_row(&h, row1, bits);
        CHECK_EQ_INT(bits[0], 0x00);
        CHECK_EQ_INT(ht_peek_err(&h, 0, 0), -7 - 17);
        CHECK_EQ_INT(ht_peek_err(&h, 0, 1), -39 - 10);
        CHECK_EQ_INT(ht_peek_err(&h, 0, 2), -23);
        ht_free(&h);
    }

    /* -- the tone curve lifts the midtones, keeps the ends, stays in order -- */
    {
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_THRESHOLD, 4), 0);
        ht_set_gain(&h, 20);
        CHECK_EQ_INT(h.tone[0], 0);
        CHECK_EQ_INT(h.tone[255], 255);
        CHECK(h.tone[128] > 160 && h.tone[128] < 180);   /* 50% ink asks for ~33% dots */
        for (int v = 1; v < 256; v++) CHECK(h.tone[v] >= h.tone[v - 1]);
        /* a 50% gray through threshold now prints paper, as it should: the
           curve says it reads as 50% only with a third of the dots */
        const uint8_t gray[4] = { 128, 128, 128, 128 };
        uint8_t bits[1] = { 0xFF };
        ht_row(&h, gray, bits);
        CHECK_EQ_INT(bits[0], 0x00);
        ht_set_gain(&h, 0);
        CHECK_EQ_INT(h.tone[128], 128);
        ht_free(&h);
    }

    /* -- blue noise: 16 cells per level, so a flat tint prints its share -- */
    {
        int count[256] = { 0 };
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64; x++) count[ht_bluenoise_at(x, y)]++;
        for (int v = 0; v < 256; v++) CHECK_EQ_INT(count[v], 16);

        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_BLUENOISE, 64), 0);
        uint8_t gray[64], bits[8];
        memset(gray, 64, sizeof gray);                /* 75% ink */
        int dots = 0;
        for (int y = 0; y < 64; y++) {
            ht_row(&h, gray, bits);
            for (int b = 0; b < 8; b++) dots += __builtin_popcount(bits[b]);
        }
        CHECK_EQ_INT(dots, (256 - 64) * 16);
        ht_free(&h);

        /* the tile upside down is its complement: a light tint on each never
           shares a dot */
        ht_t a, b;
        CHECK_EQ_INT(ht_init(&a, HT_BLUENOISE, 64), 0);
        CHECK_EQ_INT(ht_init(&b, HT_BLUENOISE, 64), 0);
        ht_set_phase(&b, 0, 0, 1);
        memset(gray, 200, sizeof gray);               /* 22% ink each */
        uint8_t ba[8], bb[8];
        int shared = 0, da = 0;
        for (int y = 0; y < 64; y++) {
            ht_row(&a, gray, ba);
            ht_row(&b, gray, bb);
            for (int i = 0; i < 8; i++) { shared += __builtin_popcount(ba[i] & bb[i]);
                                          da += __builtin_popcount(ba[i]); }
        }
        CHECK_EQ_INT(shared, 0);
        CHECK_EQ_INT(da, 56 * 16);
        ht_free(&a); ht_free(&b);
    }

    /* -- dot-off-dot: a second ink keeps off the first's dots where it can --
     *
     * Two 25% tints through Floyd-Steinberg, 256 wide and 64 rows. Blind to
     * each other they share about a sixteenth of the cells; told to avoid,
     * far fewer, and the second still prints as many dots as it did blind.
     */
    {
        enum { W = 256, R = 64, NB = W / 8 };
        uint8_t gray[W]; memset(gray, 191, sizeof gray);
        uint8_t ba[NB], bb[NB];
        int shared_blind = 0, shared_avoid = 0, dots_blind = 0, dots_avoid = 0;
        ht_t a, b;
        CHECK_EQ_INT(ht_init(&a, HT_FLOYD, W), 0);
        CHECK_EQ_INT(ht_init(&b, HT_FLOYD, W), 0);
        for (int y = 0; y < R; y++) {
            ht_row(&a, gray, ba);
            ht_row(&b, gray, bb);
            for (int i = 0; i < NB; i++) { shared_blind += __builtin_popcount(ba[i] & bb[i]);
                                           dots_blind += __builtin_popcount(bb[i]); }
        }
        ht_free(&a); ht_free(&b);
        CHECK_EQ_INT(ht_init(&a, HT_FLOYD, W), 0);
        CHECK_EQ_INT(ht_init(&b, HT_FLOYD, W), 0);
        for (int y = 0; y < R; y++) {
            ht_row(&a, gray, ba);
            ht_row_avoid(&b, gray, ba, bb);
            for (int i = 0; i < NB; i++) { shared_avoid += __builtin_popcount(ba[i] & bb[i]);
                                           dots_avoid += __builtin_popcount(bb[i]); }
        }
        ht_free(&a); ht_free(&b);
        CHECK(shared_avoid * 4 < shared_blind);
        CHECK(dots_avoid > dots_blind * 9 / 10 && dots_avoid < dots_blind * 11 / 10);
        printf("  dot-off-dot: %d shared blind, %d avoiding; %d dots blind, %d avoiding\n",
               shared_blind, shared_avoid, dots_blind, dots_avoid);
    }


    /* -- Apple's screen: the name, the tables, the tile, which way round -- */
    {
        ht_algo_t a;
        CHECK_EQ_INT(ht_algo_from_name("apple", &a), 0);
        CHECK_EQ_INT(a, HT_APPLE);
        CHECK_EQ_INT(HT_APPLE, HT_BLUENOISE + 1);        /* stored by name, but last all the same */
    }

    /* The tables are Apple's, read out of the drivers by
     * tools/stylewriter/mktables.py (checked against TailTalk's copy with its
     * --compare). Pin the ends and a few values so a bad regeneration shows. */
    {
        CHECK_EQ_INT(apple_screen16[0], 0x23);
        CHECK_EQ_INT(apple_screen16[1], 0x6B);
        CHECK_EQ_INT(apple_screen16[15], 0x5B);
        CHECK_EQ_INT(apple_screen16[255], 0x1B);
        int lo = 255, hi = 0;
        for (int i = 0; i < 256; i++) {
            if (apple_screen16[i] < lo) lo = apple_screen16[i];
            if (apple_screen16[i] > hi) hi = apple_screen16[i];
        }
        CHECK_EQ_INT(lo, 1);
        CHECK_EQ_INT(hi, 254);                           /* the threshold the black ink mirrors about */

        const uint8_t *curves[4] = { apple_tone_cs[0], apple_tone_cs[1],
                                     apple_tone_sw[0], apple_tone_sw[1] };
        for (int c = 0; c < 4; c++) {
            CHECK_EQ_INT(curves[c][0], 0);
            CHECK_EQ_INT(curves[c][255], 255);
            for (int v = 1; v < 256; v++) CHECK(curves[c][v] >= curves[c][v - 1]);
        }
        CHECK_EQ_INT(apple_tone_cs[0][1], 0x03);         /* the 2200's pattern curve starts steep */
        CHECK_EQ_INT(apple_tone_sw[0][1], 0x04);         /* the 1500's a little steeper still */

        /* a half tint prints about half the cells: no cell value is favoured */
        int under = 0;
        for (int i = 0; i < 256; i++) under += apple_screen16[i] < 128;
        CHECK(under > 120 && under < 136);
    }

    /* The tile, every 16 pixels and 16 rows. A flat amount of 128 (gray 127)
     * inks the cells whose threshold is under 128, and bare paper inks none,
     * full black all, whatever the tile. */
    {
        enum { W = 40, R = 40, NB = W / 8 };
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_APPLE, W), 0);
        uint8_t gray[W], bits[NB];
        memset(gray, 127, sizeof gray);
        int wrong = 0, inked = 0;
        for (int y = 0; y < R; y++) {
            uint8_t want[NB];
            ht_row(&h, gray, bits);
            apple_row(want, W, y, 0, 0, 0, 128);
            wrong += memcmp(bits, want, NB) != 0;
            for (int i = 0; i < NB; i++) inked += __builtin_popcount(bits[i]);
        }
        CHECK_EQ_INT(wrong, 0);
        CHECK(inked > R * W * 45 / 100 && inked < R * W * 55 / 100);

        memset(gray, 255, sizeof gray);
        ht_row(&h, gray, bits);
        for (int i = 0; i < NB; i++) CHECK_EQ_INT(bits[i], 0x00);
        memset(gray, 0, sizeof gray);
        ht_row(&h, gray, bits);
        for (int i = 0; i < NB; i++) CHECK_EQ_INT(bits[i], 0xFF);
        ht_free(&h);
    }

    /* Turned over (what the black ink gets): a cell inks where its threshold
     * mirrored about 254 is under the amount. The phase starts the tile in. */
    {
        enum { W = 32, R = 20, NB = W / 8 };
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_APPLE, W), 0);
        ht_set_phase(&h, 3, 5, 1);
        uint8_t gray[W], bits[NB];
        memset(gray, 127, sizeof gray);
        int wrong = 0;
        for (int y = 0; y < R; y++) {
            uint8_t want[NB];
            ht_row(&h, gray, bits);
            apple_row(want, W, y, 3, 5, 1, 128);
            wrong += memcmp(bits, want, NB) != 0;
        }
        CHECK_EQ_INT(wrong, 0);
        ht_free(&h);
    }

    /* Mask screens carry nothing and ignore where an earlier ink printed: the
     * Apple tile keeps inks apart by turning the black one over. */
    {
        enum { W = 32, NB = W / 8 };
        ht_t a, b;
        CHECK_EQ_INT(ht_init(&a, HT_APPLE, W), 0);
        CHECK_EQ_INT(ht_init(&b, HT_APPLE, W), 0);
        uint8_t gray[W], avoid[NB], x[NB], y[NB];
        memset(gray, 100, sizeof gray);
        memset(avoid, 0xFF, sizeof avoid);
        ht_row(&a, gray, x);
        ht_row_avoid(&b, gray, avoid, y);
        CHECK_EQ_MEM(x, y, NB);
        ht_free(&a); ht_free(&b);
    }

    /* ht_set_tone: a curve ahead of the screen, NULL for none, and the dot
     * gain curve composed on top of it whichever is set first. */
    {
        enum { W = 16, NB = W / 8 };
        ht_t h;
        CHECK_EQ_INT(ht_init(&h, HT_APPLE, W), 0);
        uint8_t zero[256], half[256], gray[W], bits[NB];
        memset(zero, 0, sizeof zero);
        for (int v = 0; v < 256; v++) half[v] = (uint8_t)(v / 2 + 64);

        ht_set_tone(&h, zero);                          /* everything reads as black */
        memset(gray, 255, sizeof gray);
        ht_row(&h, gray, bits);
        for (int i = 0; i < NB; i++) CHECK_EQ_INT(bits[i], 0xFF);

        ht_set_tone(&h, NULL);                          /* identity again */
        for (int v = 0; v < 256; v++) CHECK_EQ_INT(h.tone[v], v);
        ht_row(&h, gray, bits);
        for (int i = 0; i < NB; i++) CHECK_EQ_INT(bits[i], 0x00);

        ht_t g;                                         /* the gain curve on its own */
        CHECK_EQ_INT(ht_init(&g, HT_APPLE, W), 0);
        ht_set_gain(&g, 20);

        ht_set_tone(&h, half);
        ht_set_gain(&h, 20);                            /* gain after the tone ... */
        for (int v = 0; v < 256; v++) CHECK_EQ_INT(h.tone[v], g.tone[half[v]]);
        ht_t k;                                         /* ... and before it */
        CHECK_EQ_INT(ht_init(&k, HT_APPLE, W), 0);
        ht_set_gain(&k, 20);
        ht_set_tone(&k, half);
        CHECK_EQ_MEM(k.tone, h.tone, 256);

        ht_set_gain(&h, 0);                             /* gain off leaves the tone curve */
        for (int v = 0; v < 256; v++) CHECK_EQ_INT(h.tone[v], half[v]);
        ht_set_tone(&h, NULL);
        ht_set_gain(&h, 20);                            /* and with no tone curve it is the gain alone */
        CHECK_EQ_MEM(h.tone, g.tone, 256);
        ht_free(&h); ht_free(&g); ht_free(&k);
    }

    TEST_END;
}
