// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "ink.h"

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
    for (int a = HT_THRESHOLD; a <= HT_BLUENOISE; a++) {
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

    TEST_END;
}
