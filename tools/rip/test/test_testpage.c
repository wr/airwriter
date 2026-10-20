// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "tp_scene.h"

#include <stdlib.h>

/* The still life, 432 x 88 as the colour section draws it. */
enum { SW = 432, SH = 88 };

static const uint8_t *px(const uint8_t *img, int x, int y) { return img + ((size_t)y * SW + x) * 3; }

static uint8_t *scene(void)
{
    uint8_t *img = malloc((size_t)SW * SH * 3);
    for (int y = 0; y < SH; y++) tp_scene_row(SW, SH, (uint32_t)y, img + (size_t)y * SW * 3);
    return img;
}

static void test_scene(void)
{
    uint8_t *img = scene();

    /* the top row is sky: blue the most */
    for (int x = 0; x < SW; x += 37) {
        const uint8_t *p = px(img, x, 0);
        CHECK(p[2] > p[0] + 20 && p[2] > p[1]);
    }
    /* each ball, at its centre, in its own colour */
    const uint8_t *r = px(img, 131, 35), *g = px(img, 243, 46), *b = px(img, 290, 44),
                  *y = px(img, 191, 66);
    CHECK(r[0] > r[1] + 60 && r[0] > r[2] + 60);
    CHECK(g[1] > g[0] + 60 && g[1] > g[2] + 30);
    CHECK(b[2] > b[0] + 60 && b[2] > b[1] + 30);
    CHECK(y[0] > y[2] + 80 && y[1] > y[2] + 80);
    /* the floor along the bottom is a warm gray */
    for (int x = 0; x < SW; x += 37) {
        const uint8_t *p = px(img, x, SH - 1);
        CHECK(abs(p[0] - p[1]) < 15 && abs(p[1] - p[2]) < 15 && p[0] >= p[2]);
    }
    /* and the same every time */
    uint8_t *again = scene();
    CHECK_EQ_INT(memcmp(img, again, (size_t)SW * SH * 3), 0);
    free(again);
    free(img);
}

int main(void)
{
    test_scene();
    TEST_END;
}
