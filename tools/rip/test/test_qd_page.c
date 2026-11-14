// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a page for the Personal LaserWriter 300. Its rows are cropped to the sheet's image
// area, coded, and sent as they are made in the record's screen; the same rows in Apple's 300
// screen are kept beside them. A page whose codes would pass the printer's memory budget is
// dropped and the kept page goes instead; a page that fits in neither fails, and the job goes
// on. Pages are fed through the stage's own functions, as ripjob would call them.
#include "test_util.h"
#include "qd_page.h"
#include "sw_code.h"
#include "halftone.h"
#include <stdlib.h>

size_t big_asked_bytes, big_asked_n;

enum { MAXROWS = 4000, MAXB = 300 };

typedef struct {
    int opens, prints, notes, pass_rows, fail_after;
    int open_page[4];          // the page each of the first opens named
    const qd_media_t *md;
    uint32_t pass_bytes;
    int note_page, note_apple, note_too_big;
    uint32_t note_bytes;
    int bad_decode;
    uint8_t (*rows)[MAXB];     // this pass's rows, decoded
    uint8_t first_codes[8];
    size_t  first_n;
} sink_t;

static int s_open(void *ctx, int page, const qd_media_t *md)
{
    sink_t *s = ctx;
    if (s->opens < 4) s->open_page[s->opens] = page;
    s->opens++;
    s->md = md;
    s->pass_rows = 0;
    s->pass_bytes = 0;
    return 0;
}
static int s_row(void *ctx, const uint8_t *codes, size_t n)
{
    sink_t *s = ctx;
    if (s->fail_after && s->pass_rows >= s->fail_after) return -1;
    size_t rb = s->md->img_w / 8;
    if (s->pass_rows == 0) {
        s->first_n = n < 8 ? n : 8;
        memcpy(s->first_codes, codes, s->first_n);
    }
    if (s->pass_rows < MAXROWS) {
        if (sw_decode_row(codes, n, NULL, rb, s->rows[s->pass_rows]) != n) s->bad_decode++;
    }
    s->pass_rows++;
    s->pass_bytes += (uint32_t)n;
    return 0;
}
static int s_print(void *ctx) { ((sink_t *)ctx)->prints++; return 0; }
static void s_note(void *ctx, int page, int apple, uint32_t bytes, int too_big)
{
    sink_t *s = ctx;
    s->notes++;
    s->note_page = page;
    s->note_apple = apple;
    s->note_bytes = bytes;
    s->note_too_big = too_big;
}

static sink_t *sink_new(void)
{
    sink_t *s = calloc(1, sizeof *s);
    s->rows = calloc(MAXROWS, MAXB);
    return s;
}
static void sink_free(sink_t *s) { free(s->rows); free(s); }

static qd_page_cfg_t cfg_for(sink_t *s, ht_algo_t screen, uint8_t *keep, size_t keep_cap, uint32_t budget)
{
    qd_page_cfg_t c = { .model = QD_PLW300, .screen = screen, .gain = 0, .keep = keep,
                        .keep_cap = keep_cap, .budget = budget,
                        .sink = { s_open, s_row, s_print, s_note, s } };
    return c;
}

// A page source: each row's 1bpp bits (w wide) and, for the gray row, its gray.
typedef void (*bits_fn)(uint32_t y, uint32_t w, uint8_t *bits);
typedef uint8_t (*gray_fn)(uint32_t x, uint32_t y);

static int feed(rip_stage_t *st, int index, uint32_t w, uint32_t h, bits_fn bf, gray_fn gf)
{
    if (st->page_begin(st->ctx, index, w, h)) return -100;
    uint8_t *bits = calloc((w + 7) / 8, 1), *gray = malloc(w);
    for (uint32_t y = 0; y < h; y++) {
        if (st->gray_row) {
            for (uint32_t x = 0; x < w; x++) gray[x] = gf ? gf(x, y) : 255;
            st->gray_row(st->ctx, gray, w);
        }
        memset(bits, 0, (w + 7) / 8);
        if (bf) bf(y, w, bits);
        st->row(st->ctx, bits);
    }
    free(bits);
    free(gray);
    return st->page_end(st->ctx);
}

static int all_bytes(const uint8_t *row, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++) if (row[i] != v) return 0;
    return 1;
}

// A black box from page row 400 to 2800, bytes 20 to 279.
static void box(uint32_t y, uint32_t w, uint8_t *bits)
{
    (void)w;
    if (y >= 400 && y < 2800) memset(bits + 20, 0xFF, 260);
}

static void test_small_page_goes_in_record_screen(void)
{
    sink_t *s = sink_new();
    static uint8_t keep[600000];
    big_asked_bytes = 0;
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, keep, sizeof keep, 0);
    qd_page_t *p = qd_page_new(&c);
    CHECK(p != NULL);
    CHECK(big_asked_bytes < 64 * 1024);           // the stage holds rows, not a page
    rip_stage_t st;
    qd_page_stage(p, &st);
    CHECK(st.gray_row != NULL);
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, box, NULL), 0);
    CHECK_EQ_INT(s->opens, 1);
    CHECK(s->md && !strcmp(s->md->media, "na_letter_8.5x11in"));
    CHECK_EQ_INT(s->pass_rows, 3175);
    CHECK_EQ_INT(s->prints, 1);
    CHECK_EQ_INT(s->bad_decode, 0);
    CHECK_EQ_INT(s->notes, 1);
    CHECK_EQ_INT(s->note_apple, 0);
    CHECK_EQ_INT(s->note_too_big, 0);
    CHECK_EQ_INT(s->note_bytes, s->pass_bytes);
    // Image row r is page row 62 + r.
    CHECK(all_bytes(s->rows[400 - 62 - 1], 300, 0));
    CHECK(all_bytes(s->rows[400 - 62] + 20, 260, 0xFF));
    CHECK(all_bytes(s->rows[400 - 62], 20, 0));
    CHECK(all_bytes(s->rows[2800 - 62], 300, 0));
    CHECK_EQ_INT(qd_page_failed(p), 0);
    qd_page_free(p);
    sink_free(s);
}

// Page row y is marked with its own number in its first two bytes.
static void numbered(uint32_t y, uint32_t w, uint8_t *bits)
{
    (void)w;
    bits[0] = (uint8_t)(y >> 8) | 0x80;
    bits[1] = (uint8_t)y;
}

static void test_rows_cropped_to_image_area(void)
{
    sink_t *s = sink_new();
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, NULL, 0, 0);
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, numbered, NULL), 0);
    CHECK_EQ_INT(s->pass_rows, 3175);
    CHECK_EQ_INT(s->rows[0][0], 0x80);
    CHECK_EQ_INT(s->rows[0][1], 62);
    unsigned last = ((s->rows[3174][0] & 0x7F) << 8) | s->rows[3174][1];
    CHECK_EQ_INT(last, 3236);                       // row 3237 and on are below the image area
    qd_page_free(p);
    sink_free(s);
}

static uint32_t g_seed;
static uint8_t rnd(void) { g_seed = g_seed * 1103515245u + 12345u; return (uint8_t)(g_seed >> 16); }
static void noise(uint32_t y, uint32_t w, uint8_t *bits)
{
    (void)y;
    for (uint32_t i = 0; i < (w + 7) / 8; i++) bits[i] = rnd();
}
static uint8_t mid_gray(uint32_t x, uint32_t y) { (void)x; return (uint8_t)(96 + (y & 63)); }

// The page in Apple's 300 screen as the test makes it: every page row through one HT_APPLE_LW
// halftoner, rows 62 to 3236 kept, and the codes they take.
static uint8_t (*apple_rows(uint32_t w, uint32_t h, gray_fn gf, uint32_t *bytes))[MAXB]
{
    uint8_t (*rows)[MAXB] = calloc(MAXROWS, MAXB);
    ht_t ht;
    ht_init(&ht, HT_APPLE_LW, w);
    uint8_t *gray = malloc(w), *bits = calloc((w + 7) / 8, 1), codes[1024];
    *bytes = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) gray[x] = gf(x, y);
        ht_row(&ht, gray, bits);
        if (y >= 62 && y < 62 + 3175) {
            memcpy(rows[y - 62], bits, 300);
            *bytes += (uint32_t)sw_code_row(bits, NULL, 300, codes);
        }
    }
    ht_free(&ht);
    free(gray);
    free(bits);
    return rows;
}

static void test_over_budget_falls_back_to_apple(void)
{
    uint32_t apple_bytes;
    uint8_t (*want)[MAXB] = apple_rows(2400, 3300, mid_gray, &apple_bytes);
    sink_t *s = sink_new();
    static uint8_t keep[600000];
    // The budget is what the Apple-screen page takes, to the byte: it fits, the noise does not.
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, keep, sizeof keep, apple_bytes);
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    g_seed = 1;
    CHECK_EQ_INT(feed(&st, 5, 2400, 3300, noise, mid_gray), 0);
    CHECK_EQ_INT(s->opens, 2);
    // W-1144: both opens name the page, so the sink knows the second is the same page again.
    CHECK_EQ_INT(s->open_page[0], 5);
    CHECK_EQ_INT(s->open_page[1], 5);
    CHECK_EQ_INT(s->pass_rows, 3175);
    CHECK_EQ_INT(s->pass_bytes, apple_bytes);
    CHECK_EQ_INT(s->prints, 1);
    CHECK_EQ_INT(s->bad_decode, 0);
    int bad = 0;
    for (int y = 0; y < 3175; y++) bad += memcmp(s->rows[y], want[y], 300) != 0;
    CHECK_EQ_INT(bad, 0);
    CHECK_EQ_INT(s->notes, 1);
    CHECK_EQ_INT(s->note_apple, 1);
    CHECK_EQ_INT(s->note_bytes, apple_bytes);
    CHECK_EQ_INT(s->note_too_big, 0);
    CHECK_EQ_INT(qd_page_failed(p), 0);

    qd_page_free(p);
    sink_free(s);

    // One byte less and the Apple page is over too: nothing prints.
    s = sink_new();
    c = cfg_for(s, HT_ATKINSON, keep, sizeof keep, apple_bytes - 1);
    p = qd_page_new(&c);
    qd_page_stage(p, &st);
    g_seed = 1;
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, noise, mid_gray), 0);
    CHECK_EQ_INT(s->prints, 0);
    CHECK_EQ_INT(s->note_too_big, 1);
    CHECK_EQ_INT(qd_page_failed(p), 1);
    qd_page_free(p);
    free(want);
    sink_free(s);
}

static void test_both_over_fails_page_and_job_goes_on(void)
{
    sink_t *s = sink_new();
    static uint8_t keep[600000];
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, keep, sizeof keep, 100);
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    g_seed = 2;
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, noise, mid_gray), 0);
    CHECK_EQ_INT(s->prints, 0);
    CHECK_EQ_INT(qd_page_failed(p), 1);
    CHECK_EQ_INT(s->notes, 1);
    CHECK_EQ_INT(s->note_too_big, 1);
    CHECK_EQ_INT(s->note_apple, 0);
    CHECK_EQ_INT(s->note_bytes, 0);
    int opens = s->opens;
    CHECK_EQ_INT(feed(&st, 1, 2400, 3300, noise, mid_gray), 0);
    CHECK_EQ_INT(s->opens, opens + 1);              // the next page starts again
    CHECK_EQ_INT(s->note_page, 1);
    CHECK_EQ_INT(qd_page_failed(p), 2);
    qd_page_free(p);
    sink_free(s);
}

static void test_apple_record_keeps_nothing(void)
{
    sink_t *s = sink_new();
    static uint8_t keep[4096];
    memset(keep, 0xA5, sizeof keep);
    qd_page_cfg_t c = cfg_for(s, HT_APPLE, keep, sizeof keep, 0);
    CHECK_EQ_INT(qd_page_rip_screen(&c), HT_APPLE_LW);
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    CHECK(st.gray_row == NULL);
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, box, NULL), 0);
    CHECK_EQ_INT(s->prints, 1);
    CHECK_EQ_INT(s->note_apple, 1);
    CHECK(all_bytes(keep, sizeof keep, 0xA5));
    qd_page_free(p);
    sink_free(s);
}

static void test_no_keep_buffer_is_apple_only(void)
{
    sink_t *s = sink_new();
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, NULL, 0, 0);
    CHECK_EQ_INT(qd_page_rip_screen(&c), HT_APPLE_LW);
    c.keep = (uint8_t *)&c;          // any buffer: the record's own screen then
    c.keep_cap = 1;
    CHECK_EQ_INT(qd_page_rip_screen(&c), HT_ATKINSON);
    c.keep = NULL;
    c.keep_cap = 0;
    // Over the budget with nothing kept: the page fails.
    c.budget = 5000;
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    CHECK(st.gray_row == NULL);
    g_seed = 3;
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, noise, NULL), 0);
    CHECK_EQ_INT(s->prints, 0);
    CHECK_EQ_INT(s->note_too_big, 1);
    CHECK_EQ_INT(qd_page_failed(p), 1);
    qd_page_free(p);
    sink_free(s);
}

static void test_blank_page_prints(void)
{
    sink_t *s = sink_new();
    static uint8_t keep[600000];
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, keep, sizeof keep, 0);
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    CHECK_EQ_INT(feed(&st, 0, 2400, 3300, NULL, NULL), 0);
    CHECK_EQ_INT(s->opens, 1);
    CHECK_EQ_INT(s->pass_rows, 3175);
    CHECK_EQ_INT(s->pass_bytes, 3175);              // each row the one byte 80
    CHECK_EQ_INT(s->first_n, 1);
    CHECK_EQ_INT(s->first_codes[0], 0x80);
    CHECK_EQ_INT(s->prints, 1);
    qd_page_free(p);
    sink_free(s);
}

static void black(uint32_t y, uint32_t w, uint8_t *bits) { (void)y; memset(bits, 0xFF, (w + 7) / 8); }

static void test_unknown_size_uses_letter(void)
{
    sink_t *s = sink_new();
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, NULL, 0, 0);
    c.screen = HT_ATKINSON;
    static uint8_t keep[600000];
    c.keep = keep;
    c.keep_cap = sizeof keep;
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    CHECK_EQ_INT(feed(&st, 0, 1200, 1800, black, NULL), 0);
    CHECK_EQ_INT(s->opens, 1);
    CHECK(s->md && !strcmp(s->md->media, "na_letter_8.5x11in"));
    CHECK_EQ_INT(s->pass_rows, 3175);
    CHECK_EQ_INT(s->prints, 1);
    CHECK_EQ_INT(s->bad_decode, 0);
    // Page rows 62 to 1799 are the page's, cut at its 150 bytes; then white to the end.
    CHECK(all_bytes(s->rows[0], 150, 0xFF) && all_bytes(s->rows[0] + 150, 150, 0));
    CHECK(all_bytes(s->rows[1799 - 62], 150, 0xFF));
    CHECK(all_bytes(s->rows[1800 - 62], 300, 0));
    CHECK(all_bytes(s->rows[3174], 300, 0));

    // Wider than the sheet's area: cut at its 300 bytes.
    CHECK_EQ_INT(feed(&st, 1, 2550, 3300, black, NULL), 0);
    CHECK(all_bytes(s->rows[0], 300, 0xFF));
    CHECK_EQ_INT(s->bad_decode, 0);
    qd_page_free(p);
    sink_free(s);
}

// Rows whose codes add up to `target` bytes exactly: noise rows, then two rows of noise as long
// as needed with white after, then blank rows.
static uint8_t (*rows_of(uint32_t target))[MAXB]
{
    uint8_t (*rows)[MAXB] = calloc(3175, MAXB);
    uint8_t codes[1024];
    uint32_t total = 0;
    int y = 0;
    g_seed = 7;
    for (; y < 3175; y++) {
        uint8_t r[MAXB];
        for (int i = 0; i < MAXB; i++) r[i] = rnd();
        uint32_t n = (uint32_t)sw_code_row(r, NULL, MAXB, codes);
        // Stop while what is left for the two last rows (the rest blank, a byte each) is
        // between 300 and about 600 bytes: two rows of up to 300 bytes of noise can make it.
        if ((int64_t)target - total - n - (3175 - y - 3) < 300) break;
        memcpy(rows[y], r, MAXB);
        total += n;
    }
    // Two rows of m and k bytes of noise, white after; the rest blank, one byte each.
    uint8_t a[MAXB], b[MAXB];
    for (int m = 0; m <= MAXB; m++) {
        memset(a, 0, MAXB);
        g_seed = 11;
        for (int i = 0; i < m; i++) a[i] = rnd() | 1;
        uint32_t na = (uint32_t)sw_code_row(a, NULL, MAXB, codes);
        for (int k = 0; k <= MAXB; k++) {
            memset(b, 0, MAXB);
            g_seed = 13;
            for (int i = 0; i < k; i++) b[i] = rnd() | 1;
            uint32_t nb = (uint32_t)sw_code_row(b, NULL, MAXB, codes);
            if (total + na + nb + (uint32_t)(3175 - y - 2) == target) {
                memcpy(rows[y], a, MAXB);
                memcpy(rows[y + 1], b, MAXB);
                return rows;
            }
        }
    }
    free(rows);
    return NULL;
}

static uint8_t (*g_rows)[MAXB];
static void from_rows(uint32_t y, uint32_t w, uint8_t *bits)
{
    (void)w;
    if (y >= 62 && y < 62 + 3175) memcpy(bits, g_rows[y - 62], 300);
}

static void test_budget_is_500000(void)
{
    static uint8_t keep[600000];
    for (int over = 0; over < 2; over++) {
        uint32_t target = over ? 500001 : 499999;
        g_rows = rows_of(target);
        CHECK(g_rows != NULL);
        if (!g_rows) return;
        sink_t *s = sink_new();
        qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, keep, sizeof keep, 0);
        qd_page_t *p = qd_page_new(&c);
        rip_stage_t st;
        qd_page_stage(p, &st);
        CHECK_EQ_INT(feed(&st, 0, 2400, 3300, from_rows, NULL), 0);
        CHECK_EQ_INT(s->prints, 1);
        CHECK_EQ_INT(s->note_apple, over);
        CHECK_EQ_INT(s->opens, 1 + over);
        if (!over) CHECK_EQ_INT(s->note_bytes, 499999);
        else CHECK_EQ_INT(s->note_bytes, 3175);    // the kept page: white, a byte a row
        qd_page_free(p);
        sink_free(s);
        free(g_rows);
    }
}

static void test_sink_error_aborts(void)
{
    sink_t *s = sink_new();
    s->fail_after = 10;
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, NULL, 0, 0);
    qd_page_t *p = qd_page_new(&c);
    rip_stage_t st;
    qd_page_stage(p, &st);
    int rc = feed(&st, 0, 2400, 3300, box, NULL);
    CHECK(rc != 0);
    CHECK_EQ_INT(s->pass_rows, 10);
    CHECK_EQ_INT(s->prints, 0);
    qd_page_free(p);
    sink_free(s);
}

static void test_new_refuses(void)
{
    sink_t *s = sink_new();
    qd_page_cfg_t c = cfg_for(s, HT_ATKINSON, NULL, 0, 0);
    c.model = QD_NONE;
    CHECK(qd_page_new(&c) == NULL);
    c = cfg_for(s, HT_ATKINSON, NULL, 0, 0);
    c.sink.print = NULL;
    CHECK(qd_page_new(&c) == NULL);
    sink_free(s);
}

int main(void)
{
    test_small_page_goes_in_record_screen();
    test_rows_cropped_to_image_area();
    test_over_budget_falls_back_to_apple();
    test_both_over_fails_page_and_job_goes_on();
    test_apple_record_keeps_nothing();
    test_no_keep_buffer_is_apple_only();
    test_blank_page_prints();
    test_unknown_size_uses_letter();
    test_budget_is_500000();
    test_sink_error_aborts();
    test_new_refuses();
    TEST_END;
}
