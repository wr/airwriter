// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1141: how many pages of a job have printed, which a phone or Mac reads as
// job-impressions-completed. The rip says when each page's last byte has left it
// (ripjob_cfg_t.page_done); the printer's side, which may be 512 KB behind, counts a
// page only once that byte has gone to the printer (jp_pages_t).
#include "test_util.h"
#include "ripjob.h"
#include "job_pipe_rip.h"
#include "printer_caps.h"

static uint8_t g_urf[1 << 16];

// An n-page URF, w x h at 144 dpi, every pixel one gray: 8-bit gray, or 24-bit sRGB.
static size_t make_urf_n(int pages, uint32_t w, uint32_t h, uint8_t gray, int rgb)
{
    size_t n = 0;
    memcpy(g_urf, "UNIRAST\0", 8); n = 8;
    g_urf[n++] = 0; g_urf[n++] = 0; g_urf[n++] = 0; g_urf[n++] = (uint8_t)pages;
    for (int p = 0; p < pages; p++) {
        uint8_t hdr[32] = { 0 };
        hdr[0] = rgb ? 24 : 8;
        hdr[1] = rgb ? 1 : 0;
        hdr[12] = (uint8_t)(w >> 24); hdr[13] = (uint8_t)(w >> 16); hdr[14] = (uint8_t)(w >> 8); hdr[15] = (uint8_t)w;
        hdr[16] = (uint8_t)(h >> 24); hdr[17] = (uint8_t)(h >> 16); hdr[18] = (uint8_t)(h >> 8); hdr[19] = (uint8_t)h;
        hdr[22] = 0; hdr[23] = 144;
        memcpy(g_urf + n, hdr, 32); n += 32;
        for (uint32_t y = 0; y < h; y++) {
            g_urf[n++] = 0;                                  // one line
            for (uint32_t x = 0; x < w;) {
                uint32_t k = w - x < 128 ? w - x : 128;
                g_urf[n++] = (uint8_t)(k - 1);               // k pixels of one gray
                for (int c = 0; c < (rgb ? 3 : 1); c++) g_urf[n++] = gray;
                x += k;
            }
        }
    }
    return n;
}

static size_t make_urf(int pages, uint32_t w, uint32_t h, uint8_t gray)
{
    return make_urf_n(pages, w, h, gray, 0);
}

// The ImageWriter's stream, and where it stood at each page_done.
static uint8_t g_out[1 << 18];
static size_t  g_out_n;
static int     g_done_pages[16], g_done_n;
static size_t  g_done_at[16];

static void out(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (g_out_n + n <= sizeof g_out) memcpy(g_out + g_out_n, b, n);
    g_out_n += n;
}

static void page_done(void *ctx, int pages)
{
    (void)ctx;
    if (g_done_n < 16) {
        g_done_pages[g_done_n] = pages;
        g_done_at[g_done_n] = g_out_n;
    }
    g_done_n++;
}

static int rip(size_t len, ripjob_cfg_t *cfg)
{
    memsrc_t m = { g_urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    int rc = ripjob_run(&u, cfg, NULL);
    urf_free(&u);
    return rc;
}

static void reset(void)
{
    g_out_n = 0;
    g_done_n = 0;
}

// The ImageWriter's pages: each reported once its form feed, its last byte, has gone out.
static void test_imagewriter_pages(void)
{
    reset();
    size_t len = make_urf(3, 144, 24, 0);
    ripjob_cfg_t cfg = { .out = out, .dither = HT_ATKINSON, .page_done = page_done };
    CHECK_EQ_INT(rip(len, &cfg), 0);
    CHECK_EQ_INT(g_done_n, 3);
    int ffs = 0;
    size_t at = 0;
    for (int i = 0; i < 3 && i < g_done_n; i++) {
        CHECK_EQ_INT(g_done_pages[i], i + 1);
        CHECK(g_done_at[i] > 0);
        CHECK_EQ_INT(g_out[g_done_at[i] - 1], 0x0C);       // the page's form feed, just out
        for (; at < g_done_at[i]; at++) ffs += g_out[at] == 0x0C;
        CHECK(ffs >= i + 1);
    }
    CHECK_EQ_INT(g_done_at[2], g_out_n);                    // nothing after the last page

    // Colour pages the same (W-1064's encoder ends a page in black, then its form feed).
    reset();
    ripjob_cfg_t c = { .out = out, .dither = HT_ATKINSON, .colour = 1, .page_done = page_done };
    CHECK_EQ_INT(rip(make_urf_n(2, 144, 16, 128, 1), &c), 0);
    CHECK_EQ_INT(g_done_n, 2);
    CHECK_EQ_INT(g_done_pages[1], 2);
    CHECK_EQ_INT(g_out[g_done_at[0] - 1], 0x0C);
    CHECK_EQ_INT(g_done_at[1], g_out_n);
}

// A page cut short is not reported: only whole pages count.
static void test_truncated_page(void)
{
    reset();
    size_t len = make_urf(2, 144, 24, 0);
    ripjob_cfg_t cfg = { .out = out, .dither = HT_ATKINSON, .page_done = page_done };
    CHECK(rip(len - 40, &cfg) < 0);
    CHECK_EQ_INT(g_done_n, 1);
    CHECK_EQ_INT(g_done_pages[0], 1);
}

// Another printer's stage (a LaserWriter's PostScript, a StyleWriter's bands): a page is
// reported once the stage's page_end has taken it.
static int g_ends, g_end_fails_at;
static int  st_begin(void *ctx, int i, uint32_t w, uint32_t h) { (void)ctx; (void)i; (void)w; (void)h; return 0; }
static void st_row(void *ctx, const uint8_t *bits) { (void)ctx; (void)bits; }
static int  st_end(void *ctx)
{
    (void)ctx;
    g_ends++;
    return g_ends == g_end_fails_at ? -1 : 0;
}
static int g_ends_at_done[16];
static void stage_page_done(void *ctx, int pages)
{
    (void)ctx;
    if (g_done_n < 16) {
        g_done_pages[g_done_n] = pages;
        g_ends_at_done[g_done_n] = g_ends;
    }
    g_done_n++;
}

static const printer_caps_t k_laser = {
    .make_model = "Apple LaserWriter II NT", .device_id = "MFG:Apple;MDL:LaserWriter II NT;CMD:URF;",
    .urf_rs = "RS300", .res_x = 300, .res_y = 300, .media = "na_letter_8.5x11in",
    .media_x = 21590, .media_y = 27940, .ppm = 4,
    .rip_dpi_x = 300, .rip_dpi_y = 300, .max_dots = 2550, .crop_margin = 0,
};

static void test_stage_pages(void)
{
    rip_stage_t st = { .page_begin = st_begin, .row = st_row, .page_end = st_end };
    ripjob_cfg_t cfg = { .caps = &k_laser, .stage = &st, .dither = HT_ATKINSON,
                         .page_done = stage_page_done };
    reset();
    g_ends = 0;
    g_end_fails_at = 0;
    CHECK_EQ_INT(rip(make_urf(3, 144, 8, 0), &cfg), 0);
    CHECK_EQ_INT(g_done_n, 3);
    for (int i = 0; i < 3 && i < g_done_n; i++) {
        CHECK_EQ_INT(g_done_pages[i], i + 1);
        CHECK_EQ_INT(g_ends_at_done[i], i + 1);            // after its page_end, not before
    }

    // The stage fails page 2: page 1 printed, page 2 did not.
    reset();
    g_ends = 0;
    g_end_fails_at = 2;
    CHECK(rip(make_urf(3, 144, 8, 0), &cfg) < 0);
    CHECK_EQ_INT(g_done_n, 1);
    CHECK_EQ_INT(g_done_pages[0], 1);
}

// The printer's side. Page ends are noted at the printer bytes written so far; the count
// moves once that many bytes have gone to the printer, never before, and only up.
static void test_ledger(void)
{
    jp_pages_t l;
    jp_pages_reset(&l);
    CHECK_EQ_INT(jp_pages_out(&l, 0), 0);                   // nothing yet

    jp_pages_end(&l, 100, 1);
    jp_pages_end(&l, 250, 2);
    CHECK_EQ_INT(jp_pages_out(&l, 99), 0);                  // page 1's last byte not out
    CHECK_EQ_INT(jp_pages_out(&l, 100), 1);
    CHECK_EQ_INT(jp_pages_out(&l, 100), 0);                 // reported once
    CHECK_EQ_INT(jp_pages_out(&l, 200), 0);
    jp_pages_end(&l, 300, 3);
    CHECK_EQ_INT(jp_pages_out(&l, 400), 3);                 // two pages out in one chunk

    // A blank page with no bytes of its own is whole when the page before it is.
    jp_pages_reset(&l);
    jp_pages_end(&l, 0, 1);
    CHECK_EQ_INT(jp_pages_out(&l, 0), 1);

    // A rip far ahead of the printer: more page ends than marks. The newest mark moves on
    // to cover the pages after it, so they are reported late, together, never early.
    jp_pages_reset(&l);
    for (int p = 1; p <= JP_PAGE_MARKS + 10; p++) jp_pages_end(&l, (size_t)p * 10, p);
    int last = 0, reported = 0;
    for (size_t o = 0; o <= (JP_PAGE_MARKS + 10) * 10; o++) {
        int n = jp_pages_out(&l, o);
        if (!n) continue;
        CHECK(n > last);
        CHECK((size_t)n * 10 <= o);                         // never ahead of the bytes out
        last = n;
        reported++;
    }
    CHECK_EQ_INT(last, JP_PAGE_MARKS + 10);
    CHECK(reported >= JP_PAGE_MARKS - 1);                   // page by page while marks lasted

    // A new job starts at none.
    jp_pages_reset(&l);
    CHECK_EQ_INT(jp_pages_out(&l, 1000), 0);
}

// The whole of it, as job_pipe runs an ImageWriter job: the rip notes its page ends in the
// ledger as it writes, far ahead; the printer drains the bytes 512 at a time and reports.
static jp_pages_t g_l;
static size_t     g_end[16];          // where each page's form feed ends, as ripped
static void mark(void *ctx, int pages)
{
    (void)ctx;
    if (pages >= 1 && pages <= 16) g_end[pages - 1] = g_out_n;
    jp_pages_end(&g_l, g_out_n, pages);
}

static void test_pipeline(void)
{
    reset();
    jp_pages_reset(&g_l);
    ripjob_cfg_t cfg = { .out = out, .dither = HT_ATKINSON, .page_done = mark };
    CHECK_EQ_INT(rip(make_urf(4, 1152, 40, 0), &cfg), 0);   // all four ripped before any prints
    for (int p = 0; p < 4; p++) CHECK_EQ_INT(g_out[g_end[p] - 1], 0x0C);
    int last = 0, reports = 0;
    for (size_t sent = 0; sent < g_out_n;) {
        size_t k = g_out_n - sent < 512 ? g_out_n - sent : 512;
        sent += k;
        int n = jp_pages_out(&g_l, sent);
        // Every page whose form feed is out by now, and none whose isn't.
        int whole = 0;
        while (whole < 4 && g_end[whole] <= sent) whole++;
        CHECK_EQ_INT(n ? n : last, whole);
        if (n) { last = n; reports++; }
    }
    CHECK_EQ_INT(last, 4);
    CHECK(reports > 1);                                     // page by page, not all at the end
}

int main(void)
{
    test_imagewriter_pages();
    test_truncated_page();
    test_stage_pages();
    test_ledger();
    test_pipeline();
    TEST_END;
}
