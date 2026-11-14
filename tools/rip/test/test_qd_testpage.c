// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the Personal LaserWriter 300's test page, the StyleWriter's drawn at 300 dpi on
// Letter's image area (qd_testpage.h, sw_testpage.h's sw_tp_geom_t). The layout scaled to 8 in
// at 300 dpi, the six screens with Apple's 300 screen in the Apple panel, and the page through
// the 300's own page stage within its memory in every screen.
#include "test_util.h"
#include "qd_testpage.h"
#include "qd_page.h"
#include "sw_font.h"
#include "bigmem.h"
#include <stdlib.h>

size_t big_asked_bytes, big_asked_n;     // BIGMEM_COUNT (bigmem.h)

static sw_tp_geom_t g_geom;

static sw_tp_info_t info_300(ht_algo_t screen)
{
    CHECK_EQ_INT(qd_tp_geom(QD_PLW300, &g_geom), 0);
    sw_tp_info_t i = {
        .printer = "Office LaserWriter", .board = "AirWriter v0.3.0 at 10.0.1.115",
        .host = "airwriter-2434.local", .model = SW_NONE, .gain = 0, .screen = screen,
        .geom = &g_geom,
    };
    return i;
}

static void test_geom(void)
{
    sw_tp_geom_t g;
    CHECK_EQ_INT(qd_tp_geom(QD_PLW300, &g), 0);
    CHECK_EQ_INT(g.dpi, 300);
    CHECK_EQ_INT(g.sheet_w, 2400);                // Letter's image area across
    CHECK_EQ_INT(g.sheet_h, 3300);                // and the sheet's rows
    CHECK_EQ_INT(g.margin_l, 75);
    CHECK_EQ_INT(g.margin_t, 62);
    CHECK_EQ_INT(g.margin_b, 63);
    CHECK_EQ_INT(g.apple, HT_APPLE_LW);
    CHECK(g.model_name && !strcmp(g.model_name, "Personal LaserWriter 300"));
    CHECK(g.art && g.art->icon && g.art->title && g.art->sub && g.art->rgb);
    CHECK(g.captions != NULL);
    CHECK_EQ_INT(qd_tp_geom(QD_NONE, &g), -1);
}

// ---- the layout

static int inside(const sw_tp_box_t *b)
{
    return (uint32_t)b->x + b->w <= 2400 && (uint32_t)b->y + b->h <= 3175;
}

static void test_layout_300dpi(void)
{
    sw_tp_info_t info = info_300(HT_ATKINSON);
    sw_tp_layout_t l;
    sw_testpage_layout(&info, &l);
    CHECK_EQ_INT(l.colour, 0);                    // gray only
    CHECK_EQ_INT(l.ruler.x, 0);
    CHECK_EQ_INT(l.ruler.w, 2400);                // 8 in
    for (int i = 0; i < 4; i++) {
        CHECK_EQ_INT(l.square[i].w, 300);         // an inch each way
        CHECK_EQ_INT(l.square[i].h, 300);
        CHECK_EQ_INT(l.square[i].x, 600 * i);
        CHECK(inside(&l.square[i]));
    }
    CHECK(inside(&l.letterhead) && inside(&l.details) && inside(&l.photo) && inside(&l.ruler));
    CHECK(inside(&l.screen_names) && inside(&l.footer) && inside(&l.ink_ramp[3]));
    for (int i = 0; i < 11; i++) CHECK(inside(&l.gray_step[i]) && l.gray_step[i].w > 0);
    for (int i = 0; i < 6; i++) CHECK(inside(&l.panel[i]) && inside(&l.panel_ramp[i]) && l.panel[i].w > 0);
    for (int i = 1; i < 6; i++) CHECK(l.panel[i].x >= l.panel[i - 1].x + l.panel[i - 1].w);
    CHECK(l.photo.x + l.photo.w <= l.ink_ramp[3].x);
    CHECK(l.first_ink < 100);
    CHECK(l.last_ink < 3175);
    CHECK_EQ_INT(l.last_ink, l.footer.y + l.footer.h - 1);
    printf("    the 300's page: ink on rows %u to %u of 3175\n", (unsigned)l.first_ink, (unsigned)l.last_ink);
}

static void test_six_panels(void)
{
    static const ht_algo_t want[6] = { HT_ATKINSON, HT_FLOYD, HT_BLUENOISE, HT_BAYER, HT_THRESHOLD, HT_APPLE_LW };
    static const ht_algo_t sw[6] = { HT_ATKINSON, HT_FLOYD, HT_BLUENOISE, HT_BAYER, HT_THRESHOLD, HT_APPLE };
    for (int k = 0; k < 6; k++) {
        sw_tp_info_t info = info_300(sw[k]);
        sw_tp_layout_t l;
        sw_testpage_layout(&info, &l);
        for (int i = 0; i < 6; i++) CHECK_EQ_INT(l.panel_screen[i], want[i]);
        CHECK_EQ_INT(l.ticked, k);                // the record's screen; HT_APPLE ticks Apple
    }
    // The StyleWriter's keeps its own Apple screen, and ticks the same way.
    sw_tp_info_t s = { .printer = "x", .board = "y", .host = "z", .model = SW_II, .screen = HT_APPLE };
    sw_tp_layout_t l;
    sw_testpage_layout(&s, &l);
    CHECK_EQ_INT(l.panel_screen[5], HT_APPLE);
    CHECK_EQ_INT(l.ticked, 5);
}

// ---- the page's dots, as the stage is given them

typedef struct {
    int      begins, ends;
    uint32_t w, h, rows;
    uint8_t *img;                 // h rows of (w + 7) / 8
} cap_t;

static int cap_begin(void *c, int i, uint32_t w, uint32_t h)
{
    cap_t *r = c;
    (void)i;
    r->begins++;
    r->w = w;
    r->h = h;
    r->img = calloc((size_t)h, (w + 7) / 8);
    return r->img ? 0 : -1;
}
static void cap_row(void *c, const uint8_t *bits)
{
    cap_t *r = c;
    if (r->rows < r->h) memcpy(r->img + (size_t)r->rows * ((r->w + 7) / 8), bits, (r->w + 7) / 8);
    r->rows++;
}
static int cap_end(void *c) { ((cap_t *)c)->ends++; return 0; }

// A dot at (x, y) of the image area (the layout's own coordinates: y from the area's top).
static int dot(const cap_t *r, uint32_t x, uint32_t y)
{
    y += 62;
    if (x >= r->w || y >= r->h) return 0;
    return r->img[(size_t)y * ((r->w + 7) / 8) + (x >> 3)] >> (7 - (x & 7)) & 1;
}

static uint32_t ink_in(const cap_t *r, const sw_tp_box_t *b)
{
    uint32_t n = 0;
    for (uint32_t y = b->y; y < (uint32_t)b->y + b->h; y++)
        for (uint32_t x = b->x; x < (uint32_t)b->x + b->w; x++) n += (uint32_t)dot(r, x, y);
    return n;
}

// The face at 300 dpi: each pixel 3 x 3 dots, a cell of 6 pixels across. Whether `s` is drawn
// in the box, exactly (every dot of it, and none other in its cells).
static int has_text(const cap_t *r, const sw_tp_box_t *b, const char *s)
{
    const uint32_t px = 3, cell = 6 * px, gh = 7 * px;
    uint32_t bw = (uint32_t)strlen(s) * cell;
    uint8_t *bm = calloc((size_t)bw * gh, 1);
    for (uint32_t i = 0; s[i]; i++) {
        const uint8_t *g = sw_font[s[i] - 0x20];
        for (uint32_t row = 0; row < 7; row++)
            for (uint32_t col = 0; col < 5; col++)
                if (g[row] >> (4 - col) & 1)
                    for (uint32_t dy = 0; dy < px; dy++)
                        for (uint32_t dx = 0; dx < px; dx++)
                            bm[(size_t)(row * px + dy) * bw + i * cell + col * px + dx] = 1;
    }
    int found = 0;
    for (uint32_t y = b->y; !found && y + gh <= (uint32_t)b->y + b->h; y++)
        for (uint32_t x = b->x; !found && x + bw <= (uint32_t)b->x + b->w; x++) {
            int ok = 1;
            for (uint32_t j = 0; ok && j < gh; j++)
                for (uint32_t i = 0; ok && i < bw; i++)
                    if (bm[(size_t)j * bw + i] != dot(r, x + i, y + j)) ok = 0;
            found = ok;
        }
    free(bm);
    return found;
}

static void render(const sw_tp_info_t *info, cap_t *r)
{
    memset(r, 0, sizeof *r);
    rip_stage_t st = { .page_begin = cap_begin, .row = cap_row, .page_end = cap_end, .ctx = r };
    CHECK_EQ_INT(sw_testpage(info, &st), SW_TP_DONE);
}

static void test_page_dots(void)
{
    sw_tp_info_t info = info_300(HT_ATKINSON);
    sw_tp_layout_t l;
    sw_testpage_layout(&info, &l);
    cap_t r;
    render(&info, &r);
    CHECK_EQ_INT(r.begins, 1);
    CHECK_EQ_INT(r.ends, 1);
    CHECK_EQ_INT(r.w, 2400);                      // the image area's width
    CHECK_EQ_INT(r.h, 3300);                      // Letter's rows, as a client's page has
    CHECK_EQ_INT(r.rows, 3300);
    // Nothing in the 62 rows over the image area or the 63 under it.
    uint32_t outside = 0;
    for (uint32_t y = 0; y < r.h; y++)
        if (y < 62 || y >= 3300 - 63)
            for (uint32_t b = 0; b < r.w / 8; b++) outside += r.img[(size_t)y * (r.w / 8) + b] != 0;
    CHECK_EQ_INT(outside, 0);

    CHECK(has_text(&r, &l.details, "Personal LaserWriter 300"));
    CHECK(has_text(&r, &l.details, "Office LaserWriter"));
    CHECK(has_text(&r, &l.details, "Letter, 2400 x 3175 dots at 300 dpi"));
    CHECK(has_text(&r, &l.details, "75 left, 62 top, 63 bottom dots"));
    CHECK(has_text(&r, &l.details, "Atkinson"));
    CHECK(!has_text(&r, &l.details, "cartridge"));
    CHECK(has_text(&r, &l.footer, "github.com/wr/airwriter"));
    // The ruler: a tick every 30 dots, 0.1 in.
    for (uint32_t k = 0; k < 80; k++) CHECK(dot(&r, k * 30, (uint32_t)l.ruler.y + 10));
    CHECK(!dot(&r, 15, (uint32_t)l.ruler.y + 10));
    // The solid square is solid; the grid's lines are a quarter inch apart.
    CHECK_EQ_INT(ink_in(&r, &l.square[1]), 300 * 300);
    for (uint32_t k = 0; k < 4; k++) CHECK(dot(&r, l.square[0].x + 1, l.square[0].y + 75 * k));
    CHECK(!dot(&r, l.square[0].x + 30, l.square[0].y + 30));
    // Eleven steps, none lighter than the last, white to black. Atkinson, with no tone curve
    // (a 300 job's gray has none), loses the lightest and darkest steps: 10% prints white and
    // 90% black, as the same grays in a job would.
    const uint32_t area = (uint32_t)l.gray_step[0].w * l.gray_step[0].h;
    uint32_t last = 0;
    for (int j = 0; j < 11; j++) {
        uint32_t v = ink_in(&r, &l.gray_step[j]);
        if (j == 0) CHECK_EQ_INT(v, 0);
        else CHECK(v >= last);
        last = v;
    }
    CHECK_EQ_INT(last, area);
    uint32_t mid = ink_in(&r, &l.gray_step[5]);
    CHECK(mid > area * 4 / 10 && mid < area * 6 / 10);
    CHECK(ink_in(&r, &l.photo) > (uint32_t)l.photo.w * l.photo.h / 10);
    for (int i = 0; i < 6; i++) CHECK(ink_in(&r, &l.panel[i]) > (uint32_t)l.panel[i].w * l.panel[i].h / 20);
    free(r.img);

    // In Apple's 300 screen each step is darker than the last.
    info = info_300(HT_APPLE);
    render(&info, &r);
    CHECK(has_text(&r, &l.details, "Apple"));
    last = 0;
    for (int j = 0; j < 11; j++) {
        uint32_t v = ink_in(&r, &l.gray_step[j]);
        if (j == 0) CHECK_EQ_INT(v, 0);
        else CHECK(v > last);
        last = v;
    }
    free(r.img);
}

// ---- through the 300's page stage, in each screen

typedef struct { int opens, prints, notes, too_big; uint32_t rows, bytes; } count_t;

static int  c_open(void *ctx, int page, const qd_media_t *md) { (void)page; (void)md; ((count_t *)ctx)->opens++; return 0; }
static int  c_row(void *ctx, const uint8_t *codes, size_t n)
{
    (void)codes;
    count_t *c = ctx;
    c->rows++;
    c->bytes += (uint32_t)n;
    return 0;
}
static int  c_print(void *ctx) { ((count_t *)ctx)->prints++; return 0; }
static void c_note(void *ctx, int page, int apple, uint32_t bytes, int too_big)
{
    (void)page; (void)apple; (void)bytes;
    count_t *c = ctx;
    c->notes++;
    c->too_big += too_big;
}

static void test_page_through_qd_page(void)
{
    static const ht_algo_t screens[6] = { HT_ATKINSON, HT_FLOYD, HT_BLUENOISE, HT_BAYER, HT_THRESHOLD, HT_APPLE };
    static const char *const names[6] = { "Atkinson", "Floyd-Steinberg", "blue noise", "ordered", "threshold", "Apple's 300 screen" };
    for (int k = 0; k < 6; k++) {
        count_t c = { 0 };
        // As the board sends it: no page kept, its rows in the record's screen as drawn.
        qd_page_cfg_t pc = { .model = QD_PLW300, .screen = screens[k],
                             .sink = { c_open, c_row, c_print, c_note, &c } };
        qd_page_t *p = qd_page_new(&pc);
        CHECK(p != NULL);
        rip_stage_t st;
        qd_page_stage(p, &st);
        sw_tp_info_t info = info_300(screens[k]);
        CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_DONE);
        CHECK_EQ_INT(c.opens, 1);
        CHECK_EQ_INT(c.rows, 3175);
        CHECK_EQ_INT(c.prints, 1);
        CHECK_EQ_INT(c.notes, 1);
        CHECK_EQ_INT(c.too_big, 0);
        CHECK(c.bytes < 500000);
        printf("    test page in %s: %u bytes of codes\n", names[k], (unsigned)c.bytes);
        qd_page_free(p);
    }
}

// ---- memory: every buffer through bigmem.h, never a page

static void test_memory_through_bigmem(void)
{
    sw_tp_info_t info = info_300(HT_ATKINSON);
    count_t c = { 0 };
    qd_page_cfg_t pc = { .model = QD_PLW300, .screen = HT_ATKINSON,
                         .sink = { c_open, c_row, c_print, c_note, &c } };
    qd_page_t *p = qd_page_new(&pc);
    rip_stage_t st;
    qd_page_stage(p, &st);
    big_asked_bytes = big_asked_n = 0;
    CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_DONE);
    printf("    the 300's test page asks bigmem for %zu bytes in %zu requests\n", big_asked_bytes, big_asked_n);
    CHECK(big_asked_bytes > 8 * 1024);
    CHECK(big_asked_bytes < 200 * 1024);
    qd_page_free(p);
}

int main(void)
{
    test_geom();
    test_layout_300dpi();
    test_six_panels();
    test_page_dots();
    test_page_through_qd_page();
    test_memory_through_bigmem();
    TEST_END;
}
