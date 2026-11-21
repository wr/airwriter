// SPDX-License-Identifier: GPL-3.0-or-later
// W-1174: crisp text and thin lines ahead of the screen (lineart.h).
#include "test_util.h"
#include "lineart.h"
#include <stdlib.h>

#define W 96
#define H 64

static uint8_t g_in[H][W], g_out[H][W];

static void paper(uint8_t v)
{
    memset(g_in, v, sizeof g_in);
}

static void box(int x0, int y0, int x1, int y1, uint8_t v)   // x1, y1 exclusive
{
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) g_in[y][x] = v;
}

// The page through la_push and la_flush; how many rows came out.
static int run(void)
{
    la_t la;
    CHECK_EQ_INT(la_init(&la, W), 0);
    la_page(&la);
    int n = 0;
    const uint8_t *o;
    for (int y = 0; y < H; y++)
        if ((o = la_push(&la, g_in[y]))) memcpy(g_out[n++], o, W);
    while ((o = la_flush(&la))) memcpy(g_out[n++], o, W);
    la_free(&la);
    return n;
}

static void test_rows_in_order(void)
{
    paper(255);
    for (int y = 0; y < H; y++) g_in[y][y] = 250;           // a mark of each row's own
    CHECK_EQ_INT(run(), H);
    for (int y = 0; y < H; y++) CHECK_EQ_INT(g_out[y][y], 250);
    // A page shorter than the delay, and one with no rows.
    la_t la;
    CHECK_EQ_INT(la_init(&la, W), 0);
    la_page(&la);
    int n = 0;
    for (int y = 0; y < 3; y++) CHECK(la_push(&la, g_in[y]) == NULL);
    while (la_flush(&la)) n++;
    CHECK_EQ_INT(n, 3);
    la_page(&la);
    CHECK(la_flush(&la) == NULL);
    la_free(&la);
}

// Gray text with a soft edge on paper: its core black, its fringe paper, nothing in between.
static void test_dark_stroke(void)
{
    paper(255);
    box(40, 10, 43, 50, 99);                                 // a 3-dot stroke of 61% gray
    box(39, 10, 40, 50, 180);                                // its anti-aliased edges
    box(43, 10, 44, 50, 240);
    CHECK_EQ_INT(run(), H);
    for (int y = 12; y < 48; y++) {
        CHECK_EQ_INT(g_out[y][39], 255);
        for (int x = 40; x < 43; x++) CHECK_EQ_INT(g_out[y][x], 0);
        CHECK_EQ_INT(g_out[y][43], 255);
    }
}

// The same on a light tint (243, like a pale panel): crisp too.
static void test_dark_stroke_on_tint(void)
{
    paper(255);
    box(10, 5, 90, 60, 243);
    box(40, 10, 43, 50, 23);
    CHECK_EQ_INT(run(), H);
    for (int x = 40; x < 43; x++) CHECK_EQ_INT(g_out[30][x], 0);
    CHECK_EQ_INT(g_out[30][20], 243);                       // the tint itself is the screen's
}

// A 16% line three dots wide: one dot wide, unbroken, black.
static void test_light_line(void)
{
    paper(255);
    box(8, 30, 88, 33, 215);
    CHECK_EQ_INT(run(), H);
    for (int x = 10; x < 86; x++) {
        CHECK_EQ_INT(g_out[30][x], 0);
        CHECK_EQ_INT(g_out[31][x], 255);
        CHECK_EQ_INT(g_out[32][x], 255);
    }
    // Soft-edged (234, 215, 226): its darkest row.
    paper(255);
    box(8, 29, 88, 30, 234);
    box(8, 30, 88, 31, 215);
    box(8, 31, 88, 32, 226);
    CHECK_EQ_INT(run(), H);
    for (int x = 10; x < 86; x++) {
        CHECK_EQ_INT(g_out[29][x], 255);
        CHECK_EQ_INT(g_out[30][x], 0);
        CHECK_EQ_INT(g_out[31][x], 255);
    }
}

// A gray area wider than 9 dots and the thin tail off it: the screen's, as far as 2 dots out.
static void test_wide_gray(void)
{
    paper(255);
    box(20, 20, 50, 44, 116);                               // a filled icon
    box(50, 30, 60, 32, 116);                               // its tail, 2 dots thick
    CHECK_EQ_INT(run(), H);
    for (int y = 20; y < 44; y++)
        for (int x = 20; x < 50; x++) CHECK_EQ_INT(g_out[y][x], 116);
    for (int x = 50; x < 52; x++) CHECK_EQ_INT(g_out[30][x], 116);
    for (int x = 54; x < 60; x++) CHECK_EQ_INT(g_out[30][x], 0);    // past that, a thin line
}

// A black shape wider than 7 dots (a QR code's square, a bold headline's stroke): its soft
// edge crisp, its inside left to the curve.
static void test_black_wide(void)
{
    paper(255);
    box(20, 10, 40, 50, 23);                                // a 20-dot black square
    box(19, 10, 20, 50, 160);                               // its soft edges
    box(40, 10, 41, 50, 100);
    box(20, 9, 40, 10, 200);
    CHECK_EQ_INT(run(), H);
    CHECK_EQ_INT(g_out[30][19], 255);
    CHECK_EQ_INT(g_out[30][20], 23);                        // no paper within a dot: the curve's
    CHECK_EQ_INT(g_out[30][40], 0);
    CHECK_EQ_INT(g_out[9][30], 255);
    CHECK_EQ_INT(g_out[30][30], 23);
}

// A photo's pale grain straddling 236 (sky, a pale wall): no white paper beside its light
// dots, so none is made a line, and it all goes to the screen as it came.
static void test_pale_grain(void)
{
    paper(255);
    uint32_t seed = 1174;
    for (int y = 4; y < 60; y++)
        for (int x = 4; x < 92; x++) {
            seed = seed * 1103515245u + 12345u;
            g_in[y][x] = (uint8_t)(225 + (seed >> 16) % 21);   // 225 to 245
        }
    CHECK_EQ_INT(run(), H);
    int changed = 0;
    for (int y = 9; y < 55; y++)
        for (int x = 9; x < 87; x++) changed += g_out[y][x] != g_in[y][x];
    CHECK_EQ_INT(changed, 0);
}

// A 16% border three dots wide with a soft edge each side (234, 215, 215, 226), as a card's:
// one unbroken dot, the rest paper.
static void test_soft_border(void)
{
    paper(255);
    box(8, 29, 88, 30, 234);
    box(8, 30, 88, 32, 215);
    box(8, 32, 88, 33, 226);
    CHECK_EQ_INT(run(), H);
    for (int x = 12; x < 84; x++) {
        CHECK_EQ_INT(g_out[29][x], 255);
        CHECK_EQ_INT(g_out[30][x], 0);
        CHECK_EQ_INT(g_out[31][x], 255);
        CHECK_EQ_INT(g_out[32][x], 255);
    }
}

// W-1178: a faint hairline, as an ImageWriter's 1/72 in rows average one (245): no line at
// the 300's 231, a line one dot wide at the ImageWriter's 247. A lone pale dot on white paper is
// no line: it goes, rather than print as a black speck.
static int run_light(int light)
{
    la_t la;
    CHECK_EQ_INT(la_init(&la, W), 0);
    la.light = light;
    la_page(&la);
    int n = 0;
    const uint8_t *o;
    for (int y = 0; y < H; y++)
        if ((o = la_push(&la, g_in[y]))) memcpy(g_out[n++], o, W);
    while ((o = la_flush(&la))) memcpy(g_out[n++], o, W);
    la_free(&la);
    return n;
}

static void test_faint_line(void)
{
    paper(255);
    box(8, 30, 88, 31, 245);
    g_in[12][40] = 240;                                     // a lone pale dot
    CHECK_EQ_INT(run_light(231), H);
    CHECK(!memcmp(g_out, g_in, sizeof g_in));
    CHECK_EQ_INT(run_light(247), H);
    for (int x = 10; x < 86; x++) CHECK_EQ_INT(g_out[30][x], 0);
    CHECK_EQ_INT(g_out[12][40], 255);                       // paper, not a black speck
}

// A thin light line at a slant goes on along itself through a corner, not only beside it: a
// 45° line, a steep one and a shallow one all print, ends and all, at 231 as at 247 (the
// review of W-1178).
static void test_slanted_light_lines(void)
{
    static const struct { int dx, dy; } slope[] = { { 1, 1 }, { 1, 3 }, { 8, 1 } };
    for (int light = 231; light <= 247; light += 16)
        for (size_t k = 0; k < sizeof slope / sizeof slope[0]; k++) {
            paper(255);
            int n = 0;
            for (int t = 0; t < 40; t++) {
                int x, y;
                if (slope[k].dx > slope[k].dy) x = 20 + t, y = 10 + t / slope[k].dx;
                else x = 20 + t / slope[k].dy, y = 10 + t;
                if (x < W && y < H) g_in[y][x] = 210, n++;
            }
            CHECK_EQ_INT(run_light(light), H);
            int black = 0, other = 0;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    if (g_in[y][x] == 210) black += g_out[y][x] == 0;
                    else other += g_out[y][x] != 255;
                }
            CHECK_EQ_INT(black, n);
            CHECK_EQ_INT(other, 0);
        }
}

// Wide areas and photos with no paper around: untouched.
static void test_no_paper(void)
{
    paper(255);
    box(0, 0, W, H, 150);
    for (int y = 10; y < 50; y++) g_in[y][40] = 30;         // a dark thin line in a gray field
    CHECK_EQ_INT(run(), H);
    CHECK(!memcmp(g_out, g_in, sizeof g_in));
}

// W-1174: board 1's 300 has about 115 KB of PSRAM for a page once its copy is made, and the
// first build, at 81 KB here, failed there. It now asks last, and only PSRAM.
static void test_memory(void)
{
    CHECK(la_bytes(2400) <= 60 * 1024);
    la_t la;
    CHECK_EQ_INT(la_init(&la, 2400), 0);
    la_free(&la);
    CHECK_EQ_INT(la_init(&la, 0), -1);
}

int main(void)
{
    test_memory();
    test_rows_in_order();
    test_dark_stroke();
    test_dark_stroke_on_tint();
    test_light_line();
    test_wide_gray();
    test_black_wide();
    test_pale_grain();
    test_faint_line();
    test_slanted_light_lines();
    test_soft_border();
    test_no_paper();
    TEST_END;
}
