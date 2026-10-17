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

    TEST_END;
}
