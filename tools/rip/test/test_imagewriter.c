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

/* The printer's command parser, as far as our stream exercises it: ESC c and
 * ESC p take no operand, ESC T takes two digits, ESC G four digits and then
 * that many graphics bytes. In text mode NUL is ignored and CTRL-X erases the
 * current line. A NUL where a digit belongs counts as 0 -- the manual does not
 * say, and with a 1152-byte strip any reading leaves the count under 1280. */
enum { M_TEXT, M_ESC, M_DIGITS, M_GRAPHICS };
typedef struct { int st, need, gfx, cancels; unsigned count; } iwm_t;

static void iwm_feed(iwm_t *m, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        switch (m->st) {
        case M_TEXT:
            if (c == 0x1B) m->st = M_ESC;
            else if (c == 0x18) m->cancels++;
            break;
        case M_ESC:
            m->count = 0;
            if (c == 'G')      { m->st = M_DIGITS; m->need = 4; m->gfx = 1; }
            else if (c == 'T') { m->st = M_DIGITS; m->need = 2; m->gfx = 0; }
            else                 m->st = M_TEXT;
            break;
        case M_DIGITS:
            m->count = m->count * 10 + (c >= '0' && c <= '9' ? (unsigned)(c - '0') : 0);
            if (--m->need == 0) m->st = m->gfx && m->count ? M_GRAPHICS : M_TEXT;
            break;
        case M_GRAPHICS:
            if (--m->count == 0) m->st = M_TEXT;
            break;
        }
    }
}

static uint8_t page[8192];
static size_t  page_n;
static void collect_page(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (page_n + n <= sizeof page) memcpy(page + page_n, b, n);
    page_n += n;
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
        for (int i = 0; i < 16; i++) iw_row(&w, row);   /* two full strips */

        /* Strip 1 is mid-page, so it ends CR LF. Strip 2 is the page's last
         * strip, so its LF is suppressed -- only CR. */
        const uint8_t want[] = {
            0x1B,'G','0','0','0','8',
            0xFF, 0, 0, 0, 0, 0, 0, 0,
            '\r', '\n',
            0x1B,'G','0','0','0','8',
            0xFF, 0, 0, 0, 0, 0, 0, 0,
            '\r'
        };
        CHECK_EQ_INT(sink_n, sizeof want);
        CHECK_EQ_MEM(sink, want, sizeof want);

        sink_n = 0;
        iw_end_page(&w);
        CHECK_EQ_INT(sink_n, 1);
        CHECK_EQ_INT(sink[0], 0x0C);      /* form feed, or it waits for Select */
    }

    /* -- a partial strip is padded with blank rows, not dropped, and it is
     *    the page's last strip so it too gets CR without a trailing LF -- */
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
        /* strip header + 8 data + CR (no LF) + FF = 6 + 8 + 1 + 1 */
        CHECK_EQ_INT(sink_n, 16);
        CHECK_EQ_INT(sink[6], 0x01);      /* only the top pin fires */
        CHECK_EQ_INT(sink[14], '\r');
        CHECK_EQ_INT(sink[15], 0x0C);
    }

    /* -- resync is 1280 NULs then CTRL-X, the Technical Reference's recovery
     *    for a printer left mid-graphics (Cancel Current Line, p. 85) -- */
    {
        sink_n = 0;
        iw_resync(collect, NULL);
        CHECK_EQ_INT(sink_n, IW_RESYNC_BYTES);
        CHECK_EQ_INT(IW_RESYNC_BYTES, 1281);
        size_t nonzero = 0;
        for (size_t i = 0; i < IW_RESYNC_NULS; i++) nonzero += sink[i] != 0;
        CHECK_EQ_INT(nonzero, 0);
        CHECK_EQ_INT(sink[IW_RESYNC_NULS], 0x18);
    }

    /* -- the link: resync only after a stream that started and was cut -- */
    {
        iw_link_t l = { 0 };

        /* Fresh, then clean jobs: never a resync. */
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 1);
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 1);

        /* A job cut after its first byte: the next job resyncs, once. */
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 0);
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 1);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 1);

        /* ...and once that job ends clean, the one after does not. */
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 1);

        /* The resyncing job is itself cut (or its resync stalls): still
         * dirty, so the job after that resyncs again. */
        iw_link_job_start(&l);
        iw_link_need_resync(&l);
        iw_link_job_end(&l, 0);
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 1);
        iw_link_job_end(&l, 0);

        /* Jobs that send nothing change nothing: a refused job does not
         * clear the pending resync, failed or "clean". */
        iw_link_job_start(&l);
        iw_link_job_end(&l, 1);
        iw_link_job_start(&l);
        iw_link_job_end(&l, 0);
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 1);
        iw_link_job_end(&l, 1);

        /* ...nor make a clean printer dirty. */
        iw_link_job_start(&l);
        iw_link_job_end(&l, 0);
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 1);
    }

    /* -- cut a full-width page at every byte: without the resync the next
     *    job's first byte mostly lands as graphics data; with it, CTRL-X is
     *    read as a command and the next job starts in text mode -- */
    {
        iw_t w;
        page_n = 0;
        iw_init(&w, IW_MAX_DOTS, collect_page, NULL);
        iw_begin_page(&w);
        uint8_t row[IW_MAX_DOTS / 8];
        memset(row, 0xA5, sizeof row);
        for (int i = 0; i < 24; i++) iw_row(&w, row);   /* three strips */
        iw_end_page(&w);
        CHECK(page_n <= sizeof page);

        /* The whole page parses back to text mode on its own. */
        iwm_t m = { 0 };
        iwm_feed(&m, page, page_n);
        CHECK_EQ_INT(m.st, M_TEXT);

        sink_n = 0;
        iw_resync(collect, NULL);
        size_t stuck = 0, bad = 0;
        for (size_t cut = 0; cut < page_n; cut++) {
            iwm_t a = { 0 };
            iwm_feed(&a, page, cut);
            stuck += a.st == M_GRAPHICS;
            iwm_feed(&a, sink, sink_n);
            bad += !(a.st == M_TEXT && a.cancels == 1);
        }
        CHECK(stuck > page_n / 2);      /* the bug: most cuts are mid-strip */
        CHECK_EQ_INT(bad, 0);           /* the fix: every cut recovers */
    }

    TEST_END;
}
