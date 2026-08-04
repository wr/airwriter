// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "imagewriter.h"

static uint8_t sink[4096];
static size_t  sink_n;
static void collect(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (sink_n + n <= sizeof sink) memcpy(sink + sink_n, b, n);
    sink_n += n;
}

int main(void)
{
    /* -- transpose: bit 0 of a column byte is the TOP pin -- */
    {
        uint8_t r[8][1];
        const uint8_t *rows[8];
        for (int i = 0; i < 8; i++) { r[i][0] = 0x00; rows[i] = r[i]; }

        r[0][0] = 0x80;                 /* top row, leftmost dot */
        uint8_t cols[8] = { 0 };
        iw_transpose(rows, cols, 8);
        CHECK_EQ_INT(cols[0], 0x01);    /* column 0, bit 0 = top pin */
        CHECK_EQ_INT(cols[1], 0x00);

        for (int i = 0; i < 8; i++) r[i][0] = 0x00;
        r[7][0] = 0x80;                 /* bottom row, leftmost dot */
        memset(cols, 0, sizeof cols);
        iw_transpose(rows, cols, 8);
        CHECK_EQ_INT(cols[0], 0x80);    /* bit 7 = bottom pin */

        for (int i = 0; i < 8; i++) r[i][0] = 0xFF;
        memset(cols, 0, sizeof cols);
        iw_transpose(rows, cols, 8);
        for (int x = 0; x < 8; x++) CHECK_EQ_INT(cols[x], 0xFF);
    }

    /* -- page framing and the four-ASCII-digit byte count -- */
    {
        iw_t w;
        sink_n = 0;
        iw_init(&w, 8, collect, NULL);
        iw_begin_page(&w);

        /* ESC c, ESC p, ESC T 1 6 */
        const uint8_t want_begin[] = { 0x1B,'c', 0x1B,'p', 0x1B,'T','1','6' };
        CHECK_EQ_INT(sink_n, sizeof want_begin);
        CHECK_EQ_MEM(sink, want_begin, sizeof want_begin);

        sink_n = 0;
        uint8_t row[1] = { 0x80 };
        for (int i = 0; i < 8; i++) iw_row(&w, row);   /* one full strip */

        /* ESC G 0 0 0 8, then 8 column bytes, then CR LF */
        const uint8_t want_strip[] = {
            0x1B,'G','0','0','0','8',
            0xFF, 0, 0, 0, 0, 0, 0, 0,
            '\r', '\n'
        };
        CHECK_EQ_INT(sink_n, sizeof want_strip);
        CHECK_EQ_MEM(sink, want_strip, sizeof want_strip);

        sink_n = 0;
        iw_end_page(&w);
        CHECK_EQ_INT(sink_n, 1);
        CHECK_EQ_INT(sink[0], 0x0C);      /* form feed, or it waits for Select */
    }

    /* -- a partial strip is padded with blank rows, not dropped -- */
    {
        iw_t w;
        sink_n = 0;
        iw_init(&w, 8, collect, NULL);
        iw_begin_page(&w);
        sink_n = 0;

        uint8_t row[1] = { 0x80 };
        iw_row(&w, row);                  /* one row only */
        CHECK_EQ_INT(sink_n, 0);          /* nothing emitted yet */

        iw_end_page(&w);
        /* strip header + 8 data + CR LF + FF = 6 + 8 + 2 + 1 */
        CHECK_EQ_INT(sink_n, 17);
        CHECK_EQ_INT(sink[6], 0x01);      /* only the top pin fires */
        CHECK_EQ_INT(sink[16], 0x0C);
    }

    TEST_END;
}
