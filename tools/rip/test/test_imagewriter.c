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

    /* -- a blank strip sends no graphics, only the line feed it owes, and
     *    only once something prints below it: board 1 fed a white page at
     *    a line a second when every blank strip was 1152 zeros -- */
    {
        iw_t w;
        iw_init(&w, 8, collect, NULL);
        iw_begin_page(&w);
        sink_n = 0;
        uint8_t blank[1] = { 0x00 }, dot[1] = { 0x80 };
        for (int i = 0; i < 8; i++) iw_row(&w, blank);  /* strip 1: blank */
        CHECK_EQ_INT(sink_n, 0);
        for (int i = 0; i < 8; i++) iw_row(&w, dot);    /* strip 2 */
        for (int i = 0; i < 16; i++) iw_row(&w, blank); /* strips 3, 4: blank */
        for (int i = 0; i < 8; i++) iw_row(&w, dot);    /* strip 5 */
        for (int i = 0; i < 12; i++) iw_row(&w, blank); /* 6 blank, then half of 7 */
        iw_end_page(&w);
        const uint8_t want[] = {
            '\n',
            0x1B,'G','0','0','0','8', 0xFF, 0, 0, 0, 0, 0, 0, 0, '\r',
            '\n', '\n', '\n',
            0x1B,'G','0','0','0','8', 0xFF, 0, 0, 0, 0, 0, 0, 0, '\r',
            0x0C,                       /* the trailing blanks' LFs: never sent */
        };
        CHECK_EQ_INT(sink_n, sizeof want);
        CHECK_EQ_MEM(sink, want, sizeof want);

        /* A page with nothing on it is its form feed. */
        iw_begin_page(&w);
        sink_n = 0;
        for (int i = 0; i < 40; i++) iw_row(&w, blank);
        iw_end_page(&w);
        CHECK_EQ_INT(sink_n, 1);
        CHECK_EQ_INT(sink[0], 0x0C);
    }

    /* -- the colour writer: a strip no ink has a dot in sends nothing but its
     *    owed line feed, as the gray one now does -- */
    {
        static iwc_t c;
        iwc_init(&c, 8, collect, NULL);
        iwc_begin_page(&c);
        sink_n = 0;
        uint8_t none[1] = { 0x00 }, dot[1] = { 0x80 };
        const uint8_t *blank4[IW_INKS] = { none, none, none, none };
        const uint8_t *yellow[IW_INKS] = { dot, none, none, none };
        for (int i = 0; i < 8; i++) iwc_row(&c, blank4);
        CHECK_EQ_INT(sink_n, 0);
        for (int i = 0; i < 8; i++) iwc_row(&c, yellow);
        for (int i = 0; i < 8; i++) iwc_row(&c, blank4);
        iwc_end_page(&c);
        const uint8_t want[] = {
            '\n', 0x1B,'K','1',
            0x1B,'G','0','0','0','1', 0xFF, '\r',
            0x1B,'K','0', 0x0C,
        };
        CHECK_EQ_INT(sink_n, sizeof want);
        CHECK_EQ_MEM(sink, want, sizeof want);
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

        /* iw_scan agrees with the printer model at every cut of our page. */
        size_t disagree = 0;
        for (size_t cut = 0; cut <= page_n; cut++) {
            iwm_t a = { 0 };
            iwm_feed(&a, page, cut);
            iw_scan_t sc;
            iw_scan_init(&sc);
            iw_scan(&sc, page, cut);
            disagree += iw_scan_between(&sc) != (a.st == M_TEXT);
        }
        CHECK_EQ_INT(disagree, 0);
    }

    /* -- iw_scan on a stream the Mac wrote (W-1053): the AppleTalk ImageWriter
     *    7.0.1 page, which ends in text, and is mid-command inside each of its
     *    graphics strips and their counts -- */
    {
        static uint8_t mac[16384];
        FILE *f = fopen("../../fixtures/pap-iw701-page.bin", "rb");
        CHECK(f != NULL);
        size_t mn = f ? fread(mac, 1, sizeof mac, f) : 0;
        if (f) fclose(f);
        CHECK_EQ_INT(mn, 11968);

        iw_scan_t sc;
        iw_scan_init(&sc);
        iw_scan(&sc, mac, mn);
        CHECK(iw_scan_between(&sc));

        /* Byte at a time agrees with whole buffers. */
        iw_scan_t one;
        iw_scan_init(&one);
        size_t strips = 0, inside = 0, mid_strip = 0, ends_clean = 0;
        for (size_t i = 0; i < mn; i++) {
            int was = iw_scan_between(&one);
            iw_scan(&one, mac + i, 1);
            if (was && mac[i] == 0x1B && i + 1 < mn && mac[i + 1] == 'G') strips++;
            inside += !iw_scan_between(&one);
            if (i + 6 < mn && mac[i] == 0x1B && mac[i + 1] == 'G' && was) {
                /* Cut inside the count, one byte short of the strip's end
                 * (mid-command both), and exactly at its end (between). */
                size_t cnt = (size_t)(mac[i + 2] - '0') * 1000 + (size_t)(mac[i + 3] - '0') * 100 +
                             (size_t)(mac[i + 4] - '0') * 10 + (size_t)(mac[i + 5] - '0');
                iw_scan_t c;
                iw_scan_init(&c);
                iw_scan(&c, mac, i + 4);
                mid_strip += !iw_scan_between(&c);
                iw_scan_init(&c);
                iw_scan(&c, mac, i + 6 + cnt - 1);
                mid_strip += !iw_scan_between(&c);
                iw_scan_init(&c);
                iw_scan(&c, mac, i + 6 + cnt);
                ends_clean += iw_scan_between(&c);
            }
        }
        CHECK(iw_scan_between(&one));
        CHECK_EQ_INT(strips, 40);
        CHECK_EQ_INT(mid_strip, 80);
        CHECK_EQ_INT(ends_clean, 40);
        CHECK(inside > mn / 2);          /* graphics are most of the page */

        /* The other argument shapes. */
        static const struct { const char *s; size_t n; int between; } cases[] = {
            { "\x1bV0003", 6, 0 },        /* ESC V nnnn c: the c is owed */
            { "\x1bV0003x", 7, 1 },
            { "\x1bR005", 5, 0 },
            { "\x1bR005*", 6, 1 },
            { "\x1bg002", 5, 0 },         /* 2 x 8 graphics bytes owed */
            { "\x1bg002ABCDEFGHIJKLMNOP", 21, 1 },
            { "\x1b(010,020", 9, 0 },     /* a tab list ends at '.' */
            { "\x1b(010,020.", 10, 1 },
            { "\x1bZ\x00", 3, 0 },
            { "\x1bZ\x00\x20", 4, 1 },
            /* ESC I: KEY 'A', WIDTH 'B' = two data bytes (0x04 and ESC are
             * data here, not the end or a command), KEY 'C', WIDTH 'a' = one
             * byte, then CTRL-D where a KEY would be. */
            { "\x1bI" "AB\x04", 5, 0 },
            { "\x1bI" "AB\x04\x1b", 6, 0 },
            { "\x1bI" "AB\x04\x1b" "Ca\x1f", 9, 0 },
            { "\x1bI" "AB\x04\x1b" "Ca\x1f\x04", 10, 1 },
            { "\x1bI\x04", 3, 1 },
            { "\x1f", 1, 0 },               /* CTRL-_ n */
            { "\x1f\x03", 2, 1 },
            { "\x1bG0000", 6, 1 },        /* an empty strip */
            { "\x1b" "c" "\x1b" "p" "\x1bT16", 8, 1 },
            { "\x1b", 1, 0 },
        };
        for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            iw_scan_t c;
            iw_scan_init(&c);
            iw_scan(&c, (const uint8_t *)cases[k].s, cases[k].n);
            if (iw_scan_between(&c) != cases[k].between)
                fprintf(stderr, "  iw_scan case %zu\n", k);
            CHECK_EQ_INT(iw_scan_between(&c), cases[k].between);
        }
    }

    /* -- self-ID (ESC ?, Table 6-7): IW, two digits of carriage width, then
     *    C with a colour ribbon in and F with a SheetFeeder -- */
    {
        static const struct { const char *s; size_t n; iw_ribbon_t want; } ids[] = {
            { "IW10CF", 6, IW_RIBBON_COLOUR },
            { "IW10C",  5, IW_RIBBON_COLOUR },
            { "IW10F",  5, IW_RIBBON_BLACK },
            { "IW10",   4, IW_RIBBON_BLACK },
            { "IW15C",  5, IW_RIBBON_COLOUR },
            { "\x00\x00IW10C", 7, IW_RIBBON_COLOUR },   /* line noise ahead of it */
            { "IW1",    3, IW_RIBBON_UNKNOWN },         /* cut short */
            { "",       0, IW_RIBBON_UNKNOWN },
            { "XX10C",  5, IW_RIBBON_UNKNOWN },
            { "IWxxC",  5, IW_RIBBON_UNKNOWN },
            { "\xC9\xD7\xB1\xB0\xC3", 5, IW_RIBBON_COLOUR },  /* "IW10C", bit 7 set */
            /* C counts only where the printer puts it, straight after the
             * width: noise read out after a black reply must not flip it */
            { "IW10FC",  6, IW_RIBBON_BLACK },
            { "IW10\x00" "C", 6, IW_RIBBON_BLACK },
            { "IW10F\r\nxC", 9, IW_RIBBON_BLACK },
        };
        for (size_t k = 0; k < sizeof ids / sizeof ids[0]; k++) {
            iw_ribbon_t got = iw_self_id_ribbon((const uint8_t *)ids[k].s, ids[k].n);
            if (got != ids[k].want) fprintf(stderr, "  self-ID case %zu\n", k);
            CHECK_EQ_INT(got, ids[k].want);
        }
    }

    /* -- a resync sent between jobs (W-1064: before asking the ribbon) settles
     *    the link, so the next job does not send another; a cut still counts -- */
    {
        iw_link_t l = { 0 };
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 0);                 /* cut short: dirty */
        CHECK_EQ_INT(l.dirty, 1);
        iw_link_resynced(&l);
        CHECK_EQ_INT(l.dirty, 0);
        iw_link_job_start(&l);
        CHECK_EQ_INT(iw_link_need_resync(&l), 0);
        iw_link_job_end(&l, 0);
        CHECK_EQ_INT(l.dirty, 1);
    }

    /* -- four-ink strips (W-1064): yellow, magenta, cyan, black, each pass
     *    trimmed to its last inked column and ended by CR with no LF -- */
    static const uint8_t begin8[] = { 0x1B,'c', 0x1B,'p', 0x1B,'T','1','6' };

    /* one_yellow_dot: one pass, and the page still ends on black */
    {
        static iwc_t w;
        uint8_t y[1] = { 0x80 }, none[1] = { 0 };
        const uint8_t *bits[IW_INKS] = { y, none, none, none };
        sink_n = 0;
        iwc_init(&w, 8, collect, NULL);
        iwc_begin_page(&w);
        iwc_row(&w, bits);
        iwc_end_page(&w);
        const uint8_t want[] = {
            0x1B,'c', 0x1B,'p', 0x1B,'T','1','6',
            0x1B,'K','1', 0x1B,'G','0','0','0','1', 0x01, '\r',
            0x1B,'K','0', 0x0C };
        CHECK_EQ_INT(sink_n, sizeof want);
        CHECK_EQ_MEM(sink, want, sizeof want);
    }

    /* pass_order_and_trim: Y M C K, each count its own last column + 1 */
    {
        static iwc_t w;
        uint8_t y[2] = { 0x10, 0x00 };      /* x = 3  */
        uint8_t m[2] = { 0x00, 0x40 };      /* x = 9  */
        uint8_t c[2] = { 0x80, 0x00 };      /* x = 0  */
        uint8_t k[2] = { 0x00, 0x01 };      /* x = 15 */
        const uint8_t *bits[IW_INKS] = { y, m, c, k };
        sink_n = 0;
        iwc_init(&w, 16, collect, NULL);
        iwc_begin_page(&w);
        iwc_row(&w, bits);
        iwc_end_page(&w);
        uint8_t want[128];
        size_t n = 0;
        memcpy(want, begin8, sizeof begin8); n += sizeof begin8;
        static const struct { char digit; const char *count; int last; } pass[] = {
            { '1', "0004", 3 }, { '2', "0010", 9 }, { '3', "0001", 0 }, { '0', "0016", 15 },
        };
        for (int p = 0; p < 4; p++) {
            want[n++] = 0x1B; want[n++] = 'K'; want[n++] = (uint8_t)pass[p].digit;
            want[n++] = 0x1B; want[n++] = 'G';
            memcpy(want + n, pass[p].count, 4); n += 4;
            for (int x = 0; x <= pass[p].last; x++) want[n++] = x == pass[p].last ? 0x01 : 0x00;
            want[n++] = '\r';
        }
        want[n++] = 0x0C;                   /* already on black: no ESC K0 */
        CHECK_EQ_INT(sink_n, n);
        CHECK_EQ_MEM(sink, want, n);
    }

    /* same_ink_not_reselected: two black strips select black once */
    {
        static iwc_t w;
        uint8_t k[1] = { 0x80 }, none[1] = { 0 };
        const uint8_t *bits[IW_INKS] = { none, none, none, k };
        sink_n = 0;
        iwc_init(&w, 8, collect, NULL);
        iwc_begin_page(&w);
        for (int i = 0; i < 16; i++) iwc_row(&w, bits);
        iwc_end_page(&w);
        const uint8_t want[] = {
            0x1B,'c', 0x1B,'p', 0x1B,'T','1','6',
            0x1B,'K','0', 0x1B,'G','0','0','0','1', 0xFF, '\r',
            '\n',         0x1B,'G','0','0','0','1', 0xFF, '\r',
            0x0C };
        CHECK_EQ_INT(sink_n, sizeof want);
        CHECK_EQ_MEM(sink, want, sizeof want);
    }

    /* blank_strip_holds_lf: a strip with no ink still moves the paper */
    {
        static iwc_t w;
        uint8_t k[1] = { 0x80 }, none[1] = { 0 };
        const uint8_t *ink[IW_INKS]   = { none, none, none, k };
        const uint8_t *blank[IW_INKS] = { none, none, none, none };
        sink_n = 0;
        iwc_init(&w, 8, collect, NULL);
        iwc_begin_page(&w);
        for (int i = 0; i < 8; i++) iwc_row(&w, i ? blank : ink);
        for (int i = 0; i < 8; i++) iwc_row(&w, blank);
        for (int i = 0; i < 8; i++) iwc_row(&w, i ? blank : ink);
        for (int i = 0; i < 8; i++) iwc_row(&w, blank);   /* trailing: no LF owed */
        iwc_end_page(&w);
        const uint8_t want[] = {
            0x1B,'c', 0x1B,'p', 0x1B,'T','1','6',
            0x1B,'K','0', 0x1B,'G','0','0','0','1', 0x01, '\r',
            '\n', '\n',   0x1B,'G','0','0','0','1', 0x01, '\r',
            0x0C };
        CHECK_EQ_INT(sink_n, sizeof want);
        CHECK_EQ_MEM(sink, want, sizeof want);
    }

    TEST_END;
}
