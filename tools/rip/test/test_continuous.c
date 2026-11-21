// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1173: a page on the ImageWriter's continuous roll prints straight through. The Top margin
// setting doesn't touch it (no rows dropped, none added), it stops at the roll's 10 ft, and it
// ends with the form feed every page ends with. A page is on the roll when its job asked for it
// or when it is longer than Letter; every other page prints as it always has.
#include "test_util.h"
#include "ripjob.h"
#include <stdlib.h>

#define W   16
#define RES 144                     /* source dpi; the printer's rows are 1/72 in, two of these */

/* A one-page URF `rows` source rows tall, white but for the printer rows in `ink` (each a
 * printer row, so source rows 2r and 2r+1). Runs of identical rows use URF's line repeat. */
static size_t make_urf(uint8_t *out, uint32_t rows, int rgb, const int *ink, int n_ink)
{
    size_t n = 0, bpp = rgb ? 3 : 1;
    memcpy(out, "UNIRAST\0", 8); n = 8;
    out[n++] = 0; out[n++] = 0; out[n++] = 0; out[n++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = rgb ? 24 : 8;
    hdr[1] = rgb ? 1 : 0;
    hdr[15] = W;
    hdr[16] = (uint8_t)(rows >> 24); hdr[17] = (uint8_t)(rows >> 16);
    hdr[18] = (uint8_t)(rows >> 8);  hdr[19] = (uint8_t)rows;
    hdr[23] = RES;
    memcpy(out + n, hdr, 32); n += 32;
    for (uint32_t y = 0; y < rows;) {
        int black = 0;
        for (int k = 0; k < n_ink; k++) black |= (uint32_t)ink[k] == y / 2;
        uint32_t run = 1;                           /* rows like this one that follow */
        while (y + run < rows && run < 256) {
            int b2 = 0;
            for (int k = 0; k < n_ink; k++) b2 |= (uint32_t)ink[k] == (y + run) / 2;
            if (b2 != black) break;
            run++;
        }
        out[n++] = (uint8_t)(run - 1);              /* this row and run - 1 more */
        out[n++] = (uint8_t)(W - 1);                /* one pixel, W times */
        for (size_t c = 0; c < bpp; c++) out[n++] = black ? 0 : 255;
        y += run;
    }
    return n;
}

static uint8_t stream[1 << 16];
static size_t  stream_n;
static void collect(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (stream_n + n <= sizeof stream) memcpy(stream + stream_n, b, n);
    stream_n += n;
}

static int dropped_lines;
static void on_log(void *ctx, const char *msg)
{
    (void)ctx;
    if (strstr(msg, "dropped")) dropped_lines++;
}

static int rip_with(const uint8_t *urf, size_t len, const printer_caps_t *caps, int colour,
                    int top, int continuous)
{
    memsrc_t m = { urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    ripjob_cfg_t cfg = { .out = collect, .dither = HT_THRESHOLD, .colour = colour, .caps = caps,
                         .top_margin = top, .continuous = continuous, .log = on_log };
    stream_n = 0;
    dropped_lines = 0;
    int rc = ripjob_run(&u, &cfg, NULL);
    urf_free(&u);
    return rc;
}

/* The printer rows of the page's first and last dots, from the stream (LFs of 8/72 in are 8
 * rows, a strip's bit r its row r); -1 for none. */
static void dot_rows(int *first, int *last)
{
    int y = 0;
    *first = *last = -1;
    for (size_t i = 0; i < stream_n && i < sizeof stream;) {
        uint8_t c = stream[i++];
        if (c == '\n') { y += 8; continue; }
        if (c != 0x1B) continue;
        uint8_t k = stream[i++];
        if (k == 'T') { i += 2; continue; }
        if (k == 'K') { i += 1; continue; }
        if (k != 'G') continue;
        int n = (stream[i] - '0') * 1000 + (stream[i + 1] - '0') * 100 +
                (stream[i + 2] - '0') * 10 + (stream[i + 3] - '0');
        i += 4;
        for (int x = 0; x < n; x++)
            for (int r = 0; r < 8; r++)
                if (stream[i + x] >> r & 1) {
                    if (*first < 0 || y + r < *first) *first = y + r;
                    if (y + r > *last) *last = y + r;
                }
        i += (size_t)n;
    }
}

static int first_row(void) { int f, l; dot_rows(&f, &l); return f; }

/* A stage that counts the rows it is given. */
static uint32_t stage_rows;
static int  st_begin(void *c, int i, uint32_t w, uint32_t h) { (void)c; (void)i; (void)w; (void)h; return 0; }
static void st_row(void *c, const uint8_t *b) { (void)c; (void)b; stage_rows++; }
static int  st_end(void *c) { (void)c; return 0; }

static uint8_t urf[1 << 20];

int main(void)
{
    const printer_caps_t *iw = &printer_caps_imagewriter;
    printer_caps_t sheets = *iw;                    /* the ImageWriter as it was: no roll */
    sheets.roll = NULL;
    static const int top_band[] = { 0 }, row80[] = { 80 };
    size_t n;

    /* A page longer than Letter (12 in) is on the roll: its first row prints at the head,
     * whatever the Top margin, gray or colour. */
    for (int rgb = 0; rgb < 2; rgb++) {
        n = make_urf(urf, 12 * RES, rgb, top_band, 1);
        static const int tops[] = { -144, -126, 0, 144 };
        for (size_t k = 0; k < sizeof tops / sizeof tops[0]; k++) {
            CHECK_EQ_INT(rip_with(urf, n, iw, rgb, tops[k], 0), 0);
            if (first_row() != 0) fprintf(stderr, "  rgb %d, top %d\n", rgb, tops[k]);
            CHECK_EQ_INT(first_row(), 0);
            CHECK(stream_n && stream[stream_n - 1] == 0x0C);
        }
        /* The same page on a printer with no roll drops its first inch, as always. */
        CHECK_EQ_INT(rip_with(urf, n, &sheets, rgb, -144, 0), 0);
        CHECK_EQ_INT(first_row(), -1);
    }

    /* A Letter page: on the roll only when the job asked for it. */
    n = make_urf(urf, 11 * RES, 0, row80, 1);
    CHECK_EQ_INT(rip_with(urf, n, iw, 0, -144, 0), 0);
    CHECK_EQ_INT(first_row(), 80 - 72);
    CHECK_EQ_INT(rip_with(urf, n, iw, 0, -144, 1), 0);
    CHECK_EQ_INT(first_row(), 80);
    CHECK_EQ_INT(rip_with(urf, n, iw, 0, 144, 1), 0);
    CHECK_EQ_INT(first_row(), 80);
    /* A job's ask means nothing to a printer with no roll. */
    CHECK_EQ_INT(rip_with(urf, n, &sheets, 0, -144, 1), 0);
    CHECK_EQ_INT(first_row(), 80 - 72);

    /* 1/8 in past Letter is still Letter; a row more is the roll. */
    n = make_urf(urf, 11 * RES + 18, 0, row80, 1);
    CHECK_EQ_INT(rip_with(urf, n, iw, 0, -144, 0), 0);
    CHECK_EQ_INT(first_row(), 80 - 72);
    n = make_urf(urf, 11 * RES + 20, 0, row80, 1);
    CHECK_EQ_INT(rip_with(urf, n, iw, 0, -144, 0), 0);
    CHECK_EQ_INT(first_row(), 80);

    /* A page shorter than Letter with no ask (the 2 x 1 in checker's size) prints as it always
     * has, byte for byte, at any Top margin. */
    {
        static uint8_t was[1 << 12];
        static const int band[] = { 10, 11, 12, 13, 14, 15, 16, 17 };
        n = make_urf(urf, RES, 0, band, 8);
        static const int tops[] = { -144, -16, 0, 16 };
        for (size_t k = 0; k < sizeof tops / sizeof tops[0]; k++) {
            CHECK_EQ_INT(rip_with(urf, n, &sheets, 0, tops[k], 0), 0);
            size_t wn = stream_n;
            CHECK(wn <= sizeof was);
            memcpy(was, stream, wn);
            CHECK_EQ_INT(rip_with(urf, n, iw, 0, tops[k], 0), 0);
            CHECK_EQ_INT(stream_n, wn);
            CHECK_EQ_MEM(stream, was, wn);
        }
    }

    /* 10 ft and no further: rows past 8640 are dropped, with one line in the log, in gray and
     * in colour. */
    for (int rgb = 0; rgb < 2; rgb++) {
        static const int ink[] = { 0, 8632, 8639, 8640, 8700 };
        n = make_urf(urf, 122 * RES, rgb, ink, 5);
        CHECK_EQ_INT(rip_with(urf, n, iw, rgb, -144, 1), 0);
        int f, l;
        dot_rows(&f, &l);
        CHECK_EQ_INT(f, 0);
        CHECK_EQ_INT(l, 8639);
        CHECK(stream_n < sizeof stream);
        CHECK(stream[stream_n - 1] == 0x0C);
        CHECK_EQ_INT(dropped_lines, 1);
        /* 10 ft exactly drops nothing and says nothing. */
        n = make_urf(urf, 120 * RES, rgb, ink, 3);
        CHECK_EQ_INT(rip_with(urf, n, iw, rgb, -144, 1), 0);
        dot_rows(&f, &l);
        CHECK_EQ_INT(l, 8639);
        CHECK_EQ_INT(dropped_lines, 0);
    }

    /* A stage (a LaserWriter's, a StyleWriter's) has no roll: it gets every row, asked or not. */
    {
        n = make_urf(urf, 12 * RES, 0, top_band, 1);
        rip_stage_t st = { .page_begin = st_begin, .row = st_row, .page_end = st_end };
        memsrc_t m = { urf, n, 0 };
        urf_t u;
        CHECK_EQ_INT(urf_open(&u, memsrc_read, &m), URF_OK);
        ripjob_cfg_t cfg = { .stage = &st, .dither = HT_THRESHOLD, .continuous = 1,
                             .top_margin = -144 };
        stage_rows = 0;
        CHECK_EQ_INT(ripjob_run(&u, &cfg, NULL), 0);
        urf_free(&u);
        CHECK_EQ_INT(stage_rows, 12 * 72);
    }

    TEST_END;
}
