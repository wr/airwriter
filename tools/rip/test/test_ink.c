// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "ink.h"
#include "apple_tables.h"

/* What Apple's tile should print at an amount of 128: a cell inks where the amount is
 * over its threshold, the threshold mirrored about 254 for black. Compared a row
 * at a time (see test_halftone.c). */
static void apple_row(uint8_t *out, int w, int y, int black)
{
    memset(out, 0, (size_t)(w + 7) / 8);
    for (int x = 0; x < w; x++) {
        int t = apple_screen16[(y & 15) * 16 + (x & 15)];
        if (black) t = 254 - t;
        if (128 > t) out[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
    }
}

/* Splits one RGB pixel and returns its four planes, Y M C K. */
static void split1(uint8_t r, uint8_t g, uint8_t b, uint8_t out[IW_INKS])
{
    const uint8_t rgb[3] = { r, g, b };
    uint8_t p[IW_INKS][1];
    uint8_t *const planes[IW_INKS] = { p[0], p[1], p[2], p[3] };
    ink_split(rgb, 1, planes);
    for (int i = 0; i < IW_INKS; i++) out[i] = p[i][0];
}

int main(void)
{
    uint8_t o[IW_INKS];

    /* Planes are halftone's convention: 255 = no ink, 0 = full ink. */
    split1(255, 255, 255, o);                       /* white: nothing */
    CHECK_EQ_INT(o[IW_YELLOW], 255); CHECK_EQ_INT(o[IW_MAGENTA], 255);
    CHECK_EQ_INT(o[IW_CYAN], 255);   CHECK_EQ_INT(o[IW_BLACK], 255);

    split1(0, 0, 0, o);                             /* black: the black band alone */
    CHECK_EQ_INT(o[IW_YELLOW], 255); CHECK_EQ_INT(o[IW_MAGENTA], 255);
    CHECK_EQ_INT(o[IW_CYAN], 255);   CHECK_EQ_INT(o[IW_BLACK], 0);

    split1(128, 128, 128, o);                       /* grey: black only */
    CHECK_EQ_INT(o[IW_YELLOW], 255); CHECK_EQ_INT(o[IW_MAGENTA], 255);
    CHECK_EQ_INT(o[IW_CYAN], 255);   CHECK_EQ_INT(o[IW_BLACK], 128);

    split1(255, 0, 0, o);                           /* red = magenta + yellow */
    CHECK_EQ_INT(o[IW_YELLOW], 0);   CHECK_EQ_INT(o[IW_MAGENTA], 0);
    CHECK_EQ_INT(o[IW_CYAN], 255);   CHECK_EQ_INT(o[IW_BLACK], 255);

    split1(255, 255, 0, o);                         /* yellow alone */
    CHECK_EQ_INT(o[IW_YELLOW], 0);   CHECK_EQ_INT(o[IW_MAGENTA], 255);
    CHECK_EQ_INT(o[IW_CYAN], 255);   CHECK_EQ_INT(o[IW_BLACK], 255);

    split1(0, 255, 255, o);                         /* cyan alone */
    CHECK_EQ_INT(o[IW_YELLOW], 255); CHECK_EQ_INT(o[IW_MAGENTA], 255);
    CHECK_EQ_INT(o[IW_CYAN], 0);     CHECK_EQ_INT(o[IW_BLACK], 255);

    split1(0, 0, 128, o);                           /* navy: cyan + magenta over black */
    CHECK_EQ_INT(o[IW_BLACK], 128);
    CHECK_EQ_INT(o[IW_YELLOW], 255); CHECK_EQ_INT(o[IW_MAGENTA], 127);
    CHECK_EQ_INT(o[IW_CYAN], 127);

    /* ink_gray: luma, white and black exact */
    {
        const uint8_t rgb[9] = { 255, 255, 255,  0, 0, 0,  255, 0, 0 };
        uint8_t g[3];
        ink_gray(rgb, 3, g);
        CHECK_EQ_INT(g[0], 255);
        CHECK_EQ_INT(g[1], 0);
        CHECK_EQ_INT(g[2], (77 * 255 + 128) >> 8);
    }

    /* ink_screens: a solid red row prints every magenta and yellow dot and
     * no cyan or black, through any screen */
    for (int a = HT_THRESHOLD; a <= HT_APPLE; a++) {
        enum { W = 64 };
        uint8_t rgb[W * 3];
        for (int x = 0; x < W; x++) { rgb[x * 3] = 255; rgb[x * 3 + 1] = 0; rgb[x * 3 + 2] = 0; }
        uint8_t b[IW_INKS][W / 8];
        uint8_t *const bits[IW_INKS] = { b[0], b[1], b[2], b[3] };
        ink_screens_t s;
        CHECK_EQ_INT(ink_screens_init(&s, (ht_algo_t)a, 0, W), 0);
        for (int y = 0; y < 3; y++) {
            ink_screens_row(&s, rgb, bits);
            int on[IW_INKS] = { 0 };
            for (int i = 0; i < IW_INKS; i++)
                for (int x = 0; x < W; x++) on[i] += !!(b[i][x >> 3] & (0x80 >> (x & 7)));
            CHECK_EQ_INT(on[IW_MAGENTA], W);
            CHECK_EQ_INT(on[IW_YELLOW], W);
            CHECK_EQ_INT(on[IW_CYAN], 0);
            CHECK_EQ_INT(on[IW_BLACK], 0);
        }
        ink_screens_free(&s);
    }

    /* ink_screens: a mid gray prints in black alone */
    {
        enum { W = 64 };
        uint8_t rgb[W * 3];
        memset(rgb, 128, sizeof rgb);
        uint8_t b[IW_INKS][W / 8];
        uint8_t *const bits[IW_INKS] = { b[0], b[1], b[2], b[3] };
        ink_screens_t s;
        CHECK_EQ_INT(ink_screens_init(&s, HT_BLUENOISE, 0, W), 0);
        ink_screens_row(&s, rgb, bits);
        int k = 0, other = 0;
        for (int x = 0; x < W / 8; x++) {
            k += __builtin_popcount(b[IW_BLACK][x]);
            other += __builtin_popcount(b[IW_CYAN][x] | b[IW_MAGENTA][x] | b[IW_YELLOW][x]);
        }
        CHECK(k > W / 4 && k < W * 3 / 4);
        CHECK_EQ_INT(other, 0);
        ink_screens_free(&s);
    }

    /* ink_screen_phase: cyan reads black's tile upside down, so a mid gray
     * through the two puts their dots in different places */
    {
        enum { W = 64 };
        uint8_t gray[W], bk[W / 8], cy[W / 8];
        memset(gray, 128, sizeof gray);
        ht_t hk, hc;
        ht_init(&hk, HT_BLUENOISE, W);
        ht_init(&hc, HT_BLUENOISE, W);
        ink_screen_phase(&hk, IW_BLACK);
        ink_screen_phase(&hc, IW_CYAN);
        ht_row(&hk, gray, bk);
        ht_row(&hc, gray, cy);
        int shared = 0;
        for (int x = 0; x < W / 8; x++) shared += __builtin_popcount(bk[x] & cy[x]);
        CHECK(shared < W / 8);
        ht_free(&hk);
        ht_free(&hc);
    }


    /* The Apple screen on a colour row: cyan, magenta and yellow read the tile
     * as it is, black reads it turned over (TailTalk's raster.rs, the
     * Color StyleWriter drivers' own placement), and each ink prints alone
     * where the others have no amount. A channel at 127 is an amount of 128. */
    {
        enum { W = 48, R = 20 };
        static const uint8_t rgb_of[IW_INKS][3] = {
            [IW_BLACK]   = { 127, 127, 127 },
            [IW_CYAN]    = { 127, 255, 255 },
            [IW_MAGENTA] = { 255, 127, 255 },
            [IW_YELLOW]  = { 255, 255, 127 },
        };
        for (int ink = 0; ink < IW_INKS; ink++) {
            uint8_t rgb[W * 3];
            for (int x = 0; x < W; x++) memcpy(rgb + x * 3, rgb_of[ink], 3);
            uint8_t b[IW_INKS][W / 8];
            uint8_t *const bits[IW_INKS] = { b[0], b[1], b[2], b[3] };
            ink_screens_t s;
            CHECK_EQ_INT(ink_screens_init(&s, HT_APPLE, 0, W), 0);
            int wrong = 0, inked = 0, others = 0;
            for (int y = 0; y < R; y++) {
                uint8_t want[W / 8];
                ink_screens_row(&s, rgb, bits);
                apple_row(want, W, y, ink == IW_BLACK);
                for (int i = 0; i < IW_INKS; i++) {
                    if (i == ink) {
                        wrong += memcmp(b[i], want, W / 8) != 0;
                        for (int k = 0; k < W / 8; k++) inked += __builtin_popcount(b[i][k]);
                    } else {
                        for (int k = 0; k < W / 8; k++) others += __builtin_popcount(b[i][k]);
                    }
                }
            }
            CHECK_EQ_INT(wrong, 0);
            CHECK(inked > R * W * 40 / 100 && inked < R * W * 60 / 100);
            CHECK_EQ_INT(others, 0);
            ink_screens_free(&s);
        }
    }

    /* ink_screen_phase for the Apple tile: the tile starts at its corner for
     * every ink, and black is the one read turned over. */
    {
        for (int ink = 0; ink < IW_INKS; ink++) {
            ht_t h;
            CHECK_EQ_INT(ht_init(&h, HT_APPLE, 16), 0);
            ink_screen_phase(&h, (iw_ink_t)ink);
            CHECK_EQ_INT(h.phase_x, 0);
            CHECK_EQ_INT(h.phase_y, 0);
            CHECK_EQ_INT(h.invert, ink == IW_BLACK);
            ht_free(&h);
        }
    }

    /* ink_screens_set_tone: a curve on R, G and B ahead of the split, not on
     * the planes after it. The threshold screen prints a plane below 128, and
     * the curve darkens 200 to 100 and 100 to 50 and nothing else. */
    {
        enum { W = 16 };
        uint8_t tone[256];
        for (int v = 0; v < 256; v++) tone[v] = (uint8_t)v;
        tone[200] = 100;
        tone[100] = 50;
        const struct { uint8_t rgb[3]; int ink[IW_INKS]; } cases[] = {
            /* untoned the planes are 200 and nothing prints; toned, a gray of 100 is black */
            { { 200, 200, 200 }, { [IW_BLACK] = 1 } },
            /* (255, 100, 255): magenta alone, its plane 100 */
            { { 255, 200, 255 }, { [IW_MAGENTA] = 1 } },
            /* (100, 100, 50): black at 100 and yellow at 255 - (100 - 50) = 205, so
             * black alone. Toned after the split, yellow's 155 would print too. */
            { { 200, 200, 100 }, { [IW_BLACK] = 1 } },
        };
        for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            uint8_t rgb[W * 3];
            for (int x = 0; x < W; x++) memcpy(rgb + x * 3, cases[c].rgb, 3);
            uint8_t b[IW_INKS][W / 8];
            uint8_t *const bits[IW_INKS] = { b[0], b[1], b[2], b[3] };
            ink_screens_t s;
            CHECK_EQ_INT(ink_screens_init(&s, HT_THRESHOLD, 0, W), 0);

            ink_screens_row(&s, rgb, bits);             /* no curve: nothing prints */
            for (int i = 0; i < IW_INKS; i++) CHECK_EQ_INT(b[i][0] | b[i][1], 0);

            ink_screens_set_tone(&s, tone);
            ink_screens_row(&s, rgb, bits);
            for (int i = 0; i < IW_INKS; i++) {
                int want = cases[c].ink[i] ? 0xFF : 0x00;
                CHECK_EQ_INT(b[i][0], want);
                CHECK_EQ_INT(b[i][1], want);
            }

            ink_screens_set_tone(&s, NULL);             /* and NULL takes it off again */
            ink_screens_row(&s, rgb, bits);
            for (int i = 0; i < IW_INKS; i++) CHECK_EQ_INT(b[i][0] | b[i][1], 0);
            ink_screens_free(&s);
        }
    }

    TEST_END;
}
