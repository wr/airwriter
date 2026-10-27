// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter's test page (sw_testpage.c, W-1103). The page is fed through the real page
// stage (sw_page.c), the frames it writes are decoded by a reader written here from the
// wire rules (sw_decode_row for the rows themselves), and the page that comes out is held to
// what the design says it has: a letterhead in the top inch, a ruler with a tick every 36
// dots (0.1 in), four squares of exactly 360 x 360 dots, six screens (Apple's among them),
// four ink swatches with a color cartridge and a gray ramp with black, and nothing where the
// model cannot print. The panels are held to a reference made here from the halftoner, the
// still life and the model's tone curve, so a panel drawn through the wrong screen fails.
#include "test_util.h"
#include "sw_testpage.h"
#include "sw_page.h"
#include "sw_code.h"
#include "sw_font.h"
#include "tp_scene.h"
#include "ink.h"
#include <stdlib.h>

size_t big_asked_bytes, big_asked_n;

// ---- the face

static void test_font(void)
{
    // Seven rows of five columns, a blank space and no other blank glyph, and no two the same.
    CHECK_EQ_INT(sizeof sw_font / sizeof sw_font[0], 95);
    for (int a = 0; a < 95; a++) {
        int ink = 0;
        for (int r = 0; r < 7; r++) {
            CHECK(sw_font[a][r] < 32);
            ink += __builtin_popcount(sw_font[a][r]);
        }
        CHECK(a == 0 ? ink == 0 : ink > 0);
        for (int b = a + 1; b < 95; b++) CHECK(memcmp(sw_font[a], sw_font[b], 7) != 0);
    }
    // Capitals and digits fill the seven rows; the lowercase letters that sit on the line leave
    // the top two rows free, and the ones that hang below it (g j p q y) reach the seventh.
    for (const char *c = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; *c; c++) {
        const uint8_t *g = sw_font[*c - 0x20];
        CHECK(g[0] != 0);                                   // a capital or a digit is seven rows tall
        CHECK(g[6] != 0);
    }
    for (const char *c = "acemnorsuvwxz"; *c; c++) {
        const uint8_t *g = sw_font[*c - 0x20];
        CHECK(g[0] == 0 && g[1] == 0);                      // x-height letters leave two rows over
        CHECK(g[6] != 0);
    }
    for (const char *c = "gjpqy"; *c; c++) CHECK(sw_font[*c - 0x20][6] != 0);
}

// ---- rows given to a stage, as the page writes them

static const uint32_t SHEET_H = SW_TP_SHEET_H;

typedef struct {
    int      begins, ends, rows, ink_rows, refuse;
    uint32_t w, h;
    uint32_t margin_t, margin_b;
    uint32_t first_ink, last_ink;       // rows with ink, in sheet rows
    int      in_margin_ink;             // ink in a row the engine cannot print
    int      past_layout_ink;           // ink in a column past the layout's 2880
    int      padding_ink;               // bits past the row's width
    int      stop_after;                // stop() answers yes after this many rows (0: never)
    int      stop_calls;
} raw_t;

static void raw_scan(raw_t *r, const uint8_t *const *planes, int n)
{
    uint32_t y = (uint32_t)r->rows++;
    int ink = 0;
    for (int p = 0; p < n; p++) {
        if (!planes[p]) continue;
        uint32_t rb = (r->w + 7) / 8;
        for (uint32_t b = 0; b < rb; b++) {
            if (!planes[p][b]) continue;
            ink = 1;
            if (b >= SW_TP_W / 8) r->past_layout_ink = 1;
            if (b == rb - 1 && (r->w & 7) && (planes[p][b] & (0xFF >> (r->w & 7)))) r->padding_ink = 1;
        }
    }
    if (!ink) return;
    if (y < r->margin_t || y >= r->h - r->margin_b) r->in_margin_ink = 1;
    if (y < r->first_ink) r->first_ink = y;
    r->last_ink = y;
    r->ink_rows++;
}

static int raw_begin(void *c, int i, uint32_t w, uint32_t h)
{
    raw_t *r = c;
    (void)i;
    r->begins++;
    r->w = w;
    r->h = h;
    return r->refuse ? -1 : 0;
}
static void raw_row(void *c, const uint8_t *bits) { const uint8_t *p[1] = { bits }; raw_scan(c, p, 1); }
static void raw_ink_row(void *c, const uint8_t *const bits[IW_INKS]) { raw_scan(c, bits, IW_INKS); }
static int  raw_end(void *c) { ((raw_t *)c)->ends++; return 0; }
static int  raw_stop(void *c)
{
    raw_t *r = c;
    r->stop_calls++;
    return r->stop_after && r->rows >= r->stop_after;
}

static void raw_stage(raw_t *r, rip_stage_t *st, int ink)
{
    memset(r, 0, sizeof *r);
    r->first_ink = UINT32_MAX;
    memset(st, 0, sizeof *st);
    st->page_begin = raw_begin;
    st->row = raw_row;
    st->page_end = raw_end;
    st->ctx = r;
    if (ink) st->ink_row = raw_ink_row;
}

static sw_tp_info_t info_for(sw_model_t m, int cart)
{
    sw_tp_info_t i = {
        .printer = "Den StyleWriter", .board = "AirWriter v0.3.0 at 10.0.1.115",
        .host = "airwriter-2434.local", .model = m, .colour_cart = cart, .gain = 0,
        .screen = HT_ATKINSON,
    };
    return i;
}

// ---- frames from the real stage, decoded

static uint8_t *g_buf;
static size_t   g_n, g_cap;

static void sink(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (g_n + n > g_cap) { g_cap = (g_n + n) * 2; g_buf = realloc(g_buf, g_cap); }
    memcpy(g_buf + g_n, f, n);
    g_n += n;
}

enum { INK_C, INK_M, INK_Y, INK_K };                 // the wire's plane order

typedef struct {
    int      pages, page_ends, rects, blocks, bad;
    int      colour;                                  // the page frame said so
    int      rect_kind_c, rect_kind_r;
    uint32_t nrows, pw, rowbytes;
    uint8_t *img[4];                                  // nrows x rowbytes each
} dec_t;

static void dec_free(dec_t *d)
{
    for (int k = 0; k < 4; k++) free(d->img[k]);
    memset(d, 0, sizeof *d);
}

static void decode(dec_t *d, sw_model_t model, uint32_t pw)
{
    const sw_spec_t *s = sw_spec(model);
    memset(d, 0, sizeof *d);
    d->nrows = SHEET_H - s->margin_t - s->margin_b;
    d->pw = pw;
    d->rowbytes = (pw + 7) / 8;
    uint8_t *tmp = malloc(d->rowbytes + 8);
    int planes = 0, plane = 0, in_rect = 0;
    uint32_t row = 0, t = 0, rb = 0;
    for (size_t pos = 0; pos + 3 <= g_n;) {
        uint8_t type = g_buf[pos];
        size_t len = g_buf[pos + 1] | (g_buf[pos + 2] << 8);
        const uint8_t *pl = g_buf + pos + 3;
        if (pos + 3 + len > g_n) { d->bad++; break; }
        switch (type) {
        case SWF_PAGE:
            d->pages++;
            d->colour = pl[0];
            for (int k = 0; k < 4; k++) d->img[k] = calloc(d->nrows, d->rowbytes);
            break;
        case SWF_RECT:
            d->rects++;
            planes = pl[0] == 'c' ? 4 : 1;
            d->rect_kind_c += pl[0] == 'c';
            d->rect_kind_r += pl[0] == 'R';
            t = (uint32_t)(pl[3] | (pl[4] << 8));
            rb = ((uint32_t)(pl[5] | (pl[6] << 8)) - (uint32_t)(pl[1] | (pl[2] << 8)) + 1 + 7) / 8;
            row = 0;
            plane = 0;
            in_rect = 1;
            break;
        case SWF_BLOCK: {
            d->blocks++;
            size_t used = 0;
            while (in_rect && used < len) {
                uint32_t y = t + row;
                if (!d->img[0] || y >= d->nrows || rb > d->rowbytes) { d->bad++; break; }
                int ink = planes == 4 ? plane : INK_K;
                uint8_t *dst = d->img[ink] + (size_t)y * d->rowbytes;
                const uint8_t *prev = (s->delta && row > 0) ? dst - d->rowbytes : NULL;
                size_t u = sw_decode_row(pl + used, len - used, prev, rb, tmp);
                if (!u) { d->bad++; break; }
                memcpy(dst, tmp, rb);
                used += u;
                if (++plane == planes) { plane = 0; row++; }
            }
            break;
        }
        case SWF_PAGE_END:
            d->page_ends++;
            in_rect = 0;
            break;
        default:
            d->bad++;
        }
        pos += 3 + len;
    }
    free(tmp);
}

// Draws the page of `info` through the real stage and decodes it.
static int page(dec_t *d, const sw_tp_info_t *info)
{
    g_n = 0;
    sw_page_cfg_t cfg = { .model = info->model, .colour_cart = info->colour_cart, .out = sink };
    sw_page_t *p = sw_page_new(&cfg);
    CHECK(p != NULL);
    if (!p) return -1;
    rip_stage_t st;
    sw_page_stage(p, &st);
    int rc = sw_testpage(info, &st);
    sw_page_free(p);
    decode(d, info->model, sw_printable_w(info->model, SW_TP_SHEET_W));
    return rc;
}

// ---- looking at a decoded page

static int bit(const dec_t *d, int ink, uint32_t x, uint32_t y)
{
    if (x >= d->pw || y >= d->nrows) return 0;
    return d->img[ink][(size_t)y * d->rowbytes + (x >> 3)] >> (7 - (x & 7)) & 1;
}

static uint32_t ink_in(const dec_t *d, int ink, const sw_tp_box_t *b)
{
    uint32_t n = 0;
    for (uint32_t y = b->y; y < (uint32_t)b->y + b->h; y++)
        for (uint32_t x = b->x; x < (uint32_t)b->x + b->w; x++) n += (uint32_t)bit(d, ink, x, y);
    return n;
}

// A column slice of a box, in tenths of its width.
static uint32_t slice_ink(const dec_t *d, int ink, const sw_tp_box_t *b, int tenth)
{
    sw_tp_box_t s = *b;
    s.x = (uint16_t)(b->x + b->w * (uint32_t)tenth / 10);
    s.w = (uint16_t)(b->w / 10);
    return ink_in(d, ink, &s);
}

// The face as the page draws it: each pixel 4 x 4 dots, a cell of 6 pixels across.
static void draw_text(uint8_t *bm, uint32_t bw, const char *s)
{
    for (uint32_t i = 0; s[i]; i++) {
        const uint8_t *g = sw_font[s[i] - 0x20];
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g[r] >> (4 - c) & 1)
                    for (int dy = 0; dy < 4; dy++)
                        for (int dx = 0; dx < 4; dx++)
                            bm[(size_t)(r * 4 + dy) * bw + i * 24 + (uint32_t)(c * 4 + dx)] = 1;
    }
}

// Whether `s` is drawn in the K plane anywhere in the box, every dot of it set and none of the
// others in its cells (so a bold word is not the plain one).
static int has_text(const dec_t *d, const sw_tp_box_t *b, const char *s)
{
    uint32_t bw = (uint32_t)strlen(s) * 24, bh = 28;
    uint8_t *bm = calloc((size_t)bw * bh, 1);
    draw_text(bm, bw, s);
    int found = 0;
    for (uint32_t y = b->y; !found && y + bh <= (uint32_t)b->y + b->h; y++)
        for (uint32_t x = b->x; !found && x + bw <= (uint32_t)b->x + b->w; x++) {
            int ok = 1;
            for (uint32_t j = 0; ok && j < bh; j++)
                for (uint32_t i = 0; ok && i < bw; i++)
                    if (bm[(size_t)j * bw + i] && !bit(d, INK_K, x + i, y + j)) ok = 0;
            // ... and exactly, so a bold or larger word does not stand in for it
            for (uint32_t j = 0; ok && j < bh; j++)
                for (uint32_t i = 0; ok && i < bw; i++)
                    if (!bm[(size_t)j * bw + i] && bit(d, INK_K, x + i, y + j)) ok = 0;
            found = ok;
        }
    free(bm);
    return found;
}

// Dots below row `last` or past the 2880 columns the layout uses.
static uint32_t ink_outside(const dec_t *d, uint32_t last)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < d->nrows; y++)
        for (uint32_t x = y > last ? 0 : 2880; x < d->pw; x++)
            for (int k = 0; k < 4; k++) n += (uint32_t)bit(d, k, x, y);
    return n;
}

// The 8-connected pieces of the page's ink whose bounding box is w x h exactly.
static int components_of_size(const dec_t *d, uint32_t w, uint32_t h, sw_tp_box_t *out, int max)
{
    size_t n = (size_t)d->pw * d->nrows;
    uint8_t *m = malloc(n);
    for (uint32_t y = 0; y < d->nrows; y++)
        for (uint32_t x = 0; x < d->pw; x++)
            m[(size_t)y * d->pw + x] = (uint8_t)(bit(d, 0, x, y) | bit(d, 1, x, y) | bit(d, 2, x, y) | bit(d, 3, x, y));
    uint32_t *stack = malloc(n * sizeof *stack);
    int found = 0;
    for (size_t s = 0; s < n; s++) {
        if (m[s] != 1) continue;
        uint32_t x0 = UINT32_MAX, y0 = UINT32_MAX, x1 = 0, y1 = 0;
        size_t sp = 0;
        stack[sp++] = (uint32_t)s;
        m[s] = 2;
        while (sp) {
            uint32_t v = stack[--sp], vx = v % d->pw, vy = v / d->pw;
            if (vx < x0) x0 = vx;
            if (vx > x1) x1 = vx;
            if (vy < y0) y0 = vy;
            if (vy > y1) y1 = vy;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int64_t nx = (int64_t)vx + dx, ny = (int64_t)vy + dy;
                    if (nx < 0 || ny < 0 || nx >= (int64_t)d->pw || ny >= (int64_t)d->nrows) continue;
                    size_t q = (size_t)ny * d->pw + (size_t)nx;
                    if (m[q] == 1) { m[q] = 2; stack[sp++] = (uint32_t)q; }
                }
        }
        if (x1 - x0 + 1 == w && y1 - y0 + 1 == h) {
            if (found < max) out[found] = (sw_tp_box_t){ (uint16_t)x0, (uint16_t)y0, (uint16_t)w, (uint16_t)h };
            found++;
        }
    }
    free(stack);
    free(m);
    return found;
}

static const char *screen_name(ht_algo_t a)
{
    switch (a) {
    case HT_ATKINSON:  return "Atkinson";
    case HT_FLOYD:     return "Floyd-Steinberg";
    case HT_BLUENOISE: return "Blue noise";
    case HT_BAYER:     return "Ordered";
    case HT_APPLE:     return "Apple";
    default:           return "Threshold";
    }
}

// ---- the layout alone

static void test_layout(void)
{
    for (int cart = 0; cart < 2; cart++) {
        sw_tp_info_t info = info_for(SW_C2500, cart);
        sw_tp_layout_t l;
        sw_testpage_layout(&info, &l);
        CHECK_EQ_INT(l.colour, cart);
        // The shortest printable area any model has (the 1200 family: 3960 - 72 - 114).
        CHECK(l.last_ink < 3774);
        CHECK(l.first_ink < 100);
        // The letterhead is in the top inch, over the details.
        CHECK(l.letterhead.y + l.letterhead.h <= 360);
        CHECK(l.letterhead.y + l.letterhead.h <= l.details.y);
        // Everything fits the 8 in across, and nothing sits on anything else.
        sw_tp_box_t all[64];
        int n = 0;
        all[n++] = l.letterhead;
        all[n++] = l.details;
        all[n++] = l.photo;
        all[n++] = l.ruler;
        all[n++] = l.footer;
        all[n++] = l.screen_names;
        for (int i = 0; i < 4; i++) all[n++] = l.square[i];
        for (int i = 0; i < 6; i++) { all[n++] = l.panel[i]; all[n++] = l.panel_ramp[i]; }
        if (cart) {
            for (int i = 0; i < 4; i++) { all[n++] = l.swatch[i]; all[n++] = l.ink_ramp[i]; }
        } else {
            all[n++] = l.ink_ramp[3];
            for (int i = 0; i < 11; i++) all[n++] = l.gray_step[i];
        }
        for (int i = 0; i < n; i++) {
            CHECK(all[i].w > 0 && all[i].h > 0);
            CHECK(all[i].x + all[i].w <= SW_TP_W);
            CHECK(all[i].y + all[i].h <= l.last_ink + 1);
            for (int j = i + 1; j < n; j++)
                CHECK(all[i].x + all[i].w <= all[j].x || all[j].x + all[j].w <= all[i].x ||
                      all[i].y + all[i].h <= all[j].y || all[j].y + all[j].h <= all[i].y);
        }
        for (int i = 0; i < 4; i++) { CHECK_EQ_INT(l.square[i].w, 360); CHECK_EQ_INT(l.square[i].h, 360); }
        // Six screens, Apple's among them, each once.
        int seen = 0;
        for (int i = 0; i < 6; i++) seen |= 1 << l.panel_screen[i];
        CHECK_EQ_INT(seen, (1 << HT_THRESHOLD) | (1 << HT_ATKINSON) | (1 << HT_FLOYD) | (1 << HT_BAYER) |
                           (1 << HT_BLUENOISE) | (1 << HT_APPLE));
        // A model-free layout: the same for every model that takes the cartridge.
        sw_tp_info_t other = info_for(SW_C2200, cart);
        sw_tp_layout_t l2;
        sw_testpage_layout(&other, &l2);
        CHECK_EQ_INT(l2.last_ink, l.last_ink);
    }
    // A model with no colour cartridge draws the gray page whatever it is told.
    sw_tp_info_t info = info_for(SW_II, 1);
    sw_tp_layout_t l;
    sw_testpage_layout(&info, &l);
    CHECK_EQ_INT(l.colour, 0);
}

// ---- nothing is drawn where the model cannot print

static void test_margins(void)
{
    static const struct { sw_model_t m; int cart; } k[] = {
        { SW_ORIGINAL, 0 }, { SW_II, 0 }, { SW_C1500, 1 }, { SW_C2200, 0 }, { SW_C2500, 1 },
    };
    for (size_t t = 0; t < sizeof k / sizeof k[0]; t++) {
        sw_tp_info_t info = info_for(k[t].m, k[t].cart);
        const sw_spec_t *s = sw_spec(k[t].m);
        raw_t r;
        rip_stage_t st;
        raw_stage(&r, &st, k[t].cart);
        r.margin_t = s->margin_t;
        r.margin_b = s->margin_b;
        CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_DONE);
        CHECK_EQ_INT(r.begins, 1);
        CHECK_EQ_INT(r.ends, 1);
        CHECK_EQ_INT(r.w, sw_printable_w(k[t].m, 3060));         // the printable width of Letter
        CHECK_EQ_INT(r.h, 3960);                                 // the whole sheet
        CHECK_EQ_INT(r.rows, 3960);
        CHECK(r.ink_rows > 1500);
        CHECK(!r.in_margin_ink);                                 // nothing the engine would cut
        CHECK(!r.past_layout_ink);                               // and nothing past 8 in across
        CHECK(!r.padding_ink);
        // The page starts under the engine's top margin and ends above its bottom one, with
        // the footer whole: its last row is the layout's.
        sw_tp_layout_t l;
        sw_testpage_layout(&info, &l);
        CHECK_EQ_INT(r.first_ink, s->margin_t + l.first_ink);
        CHECK_EQ_INT(r.last_ink, s->margin_t + l.last_ink);
        CHECK(r.last_ink < 3960 - s->margin_b);
    }
}

// ---- the page, decoded

static void test_colour_page(void)
{
    sw_tp_info_t info = info_for(SW_C2500, 1);
    info.gain = 7;
    info.screen = HT_FLOYD;
    dec_t d;
    CHECK_EQ_INT(page(&d, &info), SW_TP_DONE);
    sw_tp_layout_t l;
    sw_testpage_layout(&info, &l);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.pages, 1);
    CHECK_EQ_INT(d.page_ends, 1);
    CHECK_EQ_INT(d.colour, 1);
    CHECK(d.rect_kind_c > 3 && d.rect_kind_r == 0);
    CHECK_EQ_INT(d.nrows, 3816);
    CHECK_EQ_INT(d.pw, 2916);

    // The letterhead is in the top inch, in all four inks (the icon) and black (the words).
    CHECK(l.letterhead.y + l.letterhead.h <= 360);
    for (int k = 0; k < 4; k++) CHECK(ink_in(&d, k, &l.letterhead) > 200);
    sw_tp_box_t right = { (uint16_t)(l.letterhead.x + l.letterhead.w / 2), l.letterhead.y,
                          (uint16_t)(l.letterhead.w / 2), l.letterhead.h };
    CHECK(ink_in(&d, INK_K, &right) > 1000);                     // "Test page" and the title

    // The board's details in the face, at 4 x 4 dots a pixel.
    CHECK(has_text(&d, &l.details, "Color StyleWriter 2500, color cartridge"));
    CHECK(has_text(&d, &l.details, "Den StyleWriter"));
    CHECK(has_text(&d, &l.details, "AirWriter v0.3.0 at 10.0.1.115"));
    CHECK(has_text(&d, &l.details, "http://airwriter-2434.local"));
    CHECK(has_text(&d, &l.details, "Floyd-Steinberg, dot gain 7%"));
    CHECK(has_text(&d, &l.details, "Printer"));
    CHECK(has_text(&d, &l.details, "Letter, 2916 x 3816 dots at 360 dpi"));    // 3060 - 72 - 72, 3960 - 45 - 99
    CHECK(has_text(&d, &l.details, "72 left, 45 top, 99 bottom dots"));
    CHECK(!has_text(&d, &l.details, "No such thing"));

    // Four solid swatches, each in its ink alone.
    for (int i = 0; i < 4; i++) {
        uint32_t area = (uint32_t)l.swatch[i].w * l.swatch[i].h;
        for (int k = 0; k < 4; k++)
            CHECK_EQ_INT(ink_in(&d, k, &l.swatch[i]), k == i ? area : 0);
    }
    CHECK(l.swatch[0].y > l.details.y + l.details.h);

    // The still life in all four inks, and a ramp of each ink from nothing to all of it.
    for (int k = 0; k < 4; k++) CHECK(ink_in(&d, k, &l.photo) > 3000);
    for (int i = 0; i < 4; i++) {
        const sw_tp_box_t *b = &l.ink_ramp[i];
        uint32_t tenth = (uint32_t)b->w / 10 * b->h;
        for (int k = 0; k < 4; k++)
            if (k != i) CHECK_EQ_INT(ink_in(&d, k, b), 0);
        CHECK(slice_ink(&d, i, b, 0) < tenth / 10);
        CHECK(slice_ink(&d, i, b, 9) > tenth * 8 / 10);
        uint32_t last = 0;
        for (int s = 0; s < 10; s += 3) {
            uint32_t v = slice_ink(&d, i, b, s);
            CHECK(v >= last);
            last = v;
        }
    }

    // The six screens, each reproduced here from the halftoner, the still life, the model's
    // tone curve and the dot gain: a panel through another screen is not the same dots. The
    // drivers start Apple's tile on its second row, and black sits where it does.
    ht_t ph[6], rh[6];
    sw_tp_box_t pb = l.panel[0], rb = l.panel_ramp[0];
    int apple = -1;
    for (int p = 0; p < 6; p++) {
        ht_algo_t a = l.panel_screen[p];
        if (a == HT_APPLE) apple = p;
        CHECK_EQ_INT(l.panel[p].w, pb.w);
        CHECK_EQ_INT(l.panel[p].h, pb.h);
        CHECK_EQ_INT(l.panel_ramp[p].w, rb.w);
        ht_t *hs[2] = { &ph[p], &rh[p] };
        for (int k = 0; k < 2; k++) {
            CHECK_EQ_INT(ht_init(hs[k], a, k ? rb.w : pb.w), 0);
            ht_set_gain(hs[k], info.gain);
            ht_set_tone(hs[k], sw_tone(SW_C2500, a));
            ink_screen_phase(hs[k], IW_BLACK);
            if (a == HT_APPLE) ht_set_phase(hs[k], hs[k]->phase_x, hs[k]->phase_y + 1, hs[k]->invert);
        }
    }
    CHECK(apple >= 0);
    {
        uint8_t *rgb = malloc((size_t)pb.w * 3), *g = malloc(pb.w), *bits = calloc((pb.w + 7) / 8, 1);
        uint32_t bad[6] = { 0 };
        for (uint32_t y = 0; y < pb.h; y++) {
            tp_scene_row_aspect(pb.w, pb.h, y, rgb, (float)pb.h / (float)pb.w);
            ink_gray(rgb, pb.w, g);
            for (int p = 0; p < 6; p++) {
                ht_row(&ph[p], g, bits);
                for (uint32_t x = 0; x < pb.w; x++)
                    bad[p] += ((bits[x >> 3] >> (7 - (x & 7)) & 1) != bit(&d, INK_K, l.panel[p].x + x, l.panel[p].y + y));
            }
        }
        for (int p = 0; p < 6; p++) CHECK_EQ_INT(bad[p], 0);
        // Each ramp runs 255 to 0 across.
        for (uint32_t x = 0; x < rb.w; x++) g[x] = (uint8_t)(255.0 - 255.0 * x / (rb.w - 1));
        uint32_t rbad[6] = { 0 };
        for (uint32_t y = 0; y < rb.h; y++)
            for (int p = 0; p < 6; p++) {
                ht_row(&rh[p], g, bits);
                for (uint32_t x = 0; x < rb.w; x++)
                    rbad[p] += ((bits[x >> 3] >> (7 - (x & 7)) & 1) != bit(&d, INK_K, l.panel_ramp[p].x + x, l.panel_ramp[p].y + y));
            }
        for (int p = 0; p < 6; p++) CHECK_EQ_INT(rbad[p], 0);
        free(rgb);
        free(g);
        free(bits);
        // And the screens differ from one another: the same still life, six sets of dots.
        for (int p = 1; p < 6; p++) {
            uint32_t diff = 0;
            for (uint32_t y = 0; y < pb.h; y += 3)
                for (uint32_t x = 0; x < pb.w; x++)
                    diff += (bit(&d, INK_K, l.panel[0].x + x, l.panel[0].y + y) != bit(&d, INK_K, l.panel[p].x + x, l.panel[p].y + y));
            CHECK(diff > 500);
        }
    }
    for (int p = 0; p < 6; p++) {
        ht_free(&ph[p]);
        ht_free(&rh[p]);
        for (int k = 0; k < 3; k++) CHECK_EQ_INT(ink_in(&d, k, &l.panel[p]), 0);     // black alone
    }
    // Their names, the one in use in bold (so not as the plain face draws it) and ticked.
    for (int p = 0; p < 6; p++) {
        int on = l.panel_screen[p] == info.screen;
        sw_tp_box_t one = { l.panel[p].x, l.screen_names.y, 480, l.screen_names.h };
        CHECK_EQ_INT(has_text(&d, &one, screen_name(l.panel_screen[p])), !on);
    }

    // The ruler: an edge, and a tick every 36 dots (0.1 in), the half inch longer and the inch
    // longer than that.
    {
        const sw_tp_box_t *r = &l.ruler;
        CHECK_EQ_INT(r->x, 0);
        CHECK_EQ_INT(r->w, 2880);
        for (uint32_t y = 0; y < 4; y++)
            for (uint32_t x = 0; x < 2880; x++) CHECK(bit(&d, INK_K, x, r->y + y));       // the edge
        uint32_t len_of[3] = { 0, 0, 0 };                            // a tenth, a half, an inch
        for (uint32_t k = 0; k <= 80; k++) {
            uint32_t x = k < 80 ? k * 36 : 2878;                     // (the last one inside the line)
            uint32_t len = 0;
            while (len < 120 && bit(&d, INK_K, x, r->y + 4 + len)) len++;
            int cls = k % 10 == 0 ? 2 : k % 5 == 0 ? 1 : 0;
            if (!len_of[cls]) len_of[cls] = len;
            CHECK_EQ_INT(len, len_of[cls]);                          // every tick of a kind alike
            if (k < 80)                                              // and none between them
                for (uint32_t x2 = x + 4; x2 < x + 33; x2++) CHECK(!bit(&d, INK_K, x2, r->y + 4));
        }
        CHECK(len_of[0] >= 8);
        CHECK(len_of[1] > len_of[0] && len_of[2] > len_of[1]);
        CHECK(len_of[2] + 4 <= r->h);
        // The inches are numbered under it.
        sw_tp_box_t nums = { 0, (uint16_t)(r->y + r->h), 2880, 60 };
        CHECK(has_text(&d, &nums, "0"));
        CHECK(has_text(&d, &nums, "4"));
        CHECK(has_text(&d, &nums, "8"));
    }

    // Four squares, exactly 360 x 360 dots each, an inch, found in the ink itself.
    sw_tp_box_t sq[8];
    CHECK_EQ_INT(components_of_size(&d, 360, 360, sq, 8), 4);
    for (int i = 0; i < 4; i++) {
        int hit = 0;
        for (int j = 0; j < 4; j++) hit |= sq[j].x == l.square[i].x && sq[j].y == l.square[i].y;
        CHECK(hit);
    }
    {
        const sw_tp_box_t *s = l.square;
        CHECK_EQ_INT(ink_in(&d, INK_K, &s[1]), 360 * 360);                          // solid black
        uint32_t checker = ink_in(&d, INK_K, &s[3]);
        CHECK(checker > 360 * 360 / 2 - 400 && checker < 360 * 360 / 2 + 400);      // the finest checker
        uint32_t grid = ink_in(&d, INK_K, &s[0]);
        CHECK(grid > 1000 && grid < 360 * 360 / 4);
        uint32_t lines = ink_in(&d, INK_K, &s[2]);
        CHECK(lines > 360 * 360 / 5 && lines < 360 * 360 / 2);
        for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) CHECK_EQ_INT(ink_in(&d, k, &s[i]), 0);
    }

    // Nothing is drawn below the footer or past 8 in across.
    CHECK_EQ_INT(ink_outside(&d, l.last_ink), 0);
    CHECK(has_text(&d, &l.footer, "wells.ee"));
    CHECK(has_text(&d, &l.footer, "github.com/wr/airwriter"));
    dec_free(&d);
}

static void test_gray_page(sw_model_t m, int cart)
{
    sw_tp_info_t info = info_for(m, cart);
    info.screen = HT_APPLE;
    dec_t d;
    CHECK_EQ_INT(page(&d, &info), SW_TP_DONE);
    sw_tp_layout_t l;
    sw_testpage_layout(&info, &l);
    const sw_spec_t *s = sw_spec(m);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.page_ends, 1);
    CHECK_EQ_INT(d.colour, 0);
    CHECK(d.rect_kind_r > 3 && d.rect_kind_c == 0);                // black rects: one ink
    CHECK_EQ_INT(d.nrows, SHEET_H - s->margin_t - s->margin_b);

    // Black alone on the whole page; the ink planes are never touched.
    for (int k = 0; k < 3; k++) {
        uint32_t n = 0;
        for (size_t i = 0; i < (size_t)d.nrows * d.rowbytes; i++) n += d.img[k][i];
        CHECK_EQ_INT(n, 0);
    }
    CHECK(has_text(&d, &l.details, s->colour ? "black cartridge" : s->name));
    CHECK(has_text(&d, &l.details, s->name));
    // The margins and printable page are the family's, written out here from the design's table.
    CHECK(has_text(&d, &l.details, m == SW_II ? "Letter, 2880 x 3774 dots at 360 dpi"
                                              : "Letter, 2916 x 3816 dots at 360 dpi"));
    CHECK(has_text(&d, &l.details, m == SW_II ? "72 left, 72 top, 114 bottom dots"
                                              : "72 left, 45 top, 99 bottom dots"));
    CHECK_EQ_INT(ink_outside(&d, l.last_ink), 0);
    // A gray ramp with black, empty at one end and full at the other.
    const sw_tp_box_t *b = &l.ink_ramp[3];
    uint32_t tenth = (uint32_t)b->w / 10 * b->h;
    CHECK(slice_ink(&d, INK_K, b, 0) < tenth / 10);
    CHECK(slice_ink(&d, INK_K, b, 9) > tenth * 8 / 10);
    // Eleven steps, 0 to 100 %, each darker than the one before it.
    uint32_t last = 0;
    for (int j = 0; j < 11; j++) {
        uint32_t v = ink_in(&d, INK_K, &l.gray_step[j]);
        if (j == 0) CHECK_EQ_INT(v, 0);
        else CHECK(v > last);
        if (j == 10) CHECK_EQ_INT(v, (uint32_t)l.gray_step[j].w * l.gray_step[j].h);
        last = v;
    }
    CHECK(ink_in(&d, INK_K, &l.photo) > 3000);
    // The same ruler and squares.
    sw_tp_box_t sq[8];
    CHECK_EQ_INT(components_of_size(&d, 360, 360, sq, 8), 4);
    for (uint32_t k = 0; k < 80; k++) CHECK(bit(&d, INK_K, k * 36, l.ruler.y + 12));
    dec_free(&d);
}

// ---- how it ends

static void test_stops(void)
{
    sw_tp_info_t info = info_for(SW_C2500, 1);
    raw_t r;
    rip_stage_t st;
    raw_stage(&r, &st, 1);
    info.stop = raw_stop;
    info.stop_ctx = &r;
    r.stop_after = 500;
    CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_STOPPED);
    CHECK(r.rows >= 500 && r.rows < 540);                    // asked every 32 rows
    CHECK_EQ_INT(r.begins, 1);
    CHECK_EQ_INT(r.ends, 0);                                 // no page end follows a stop
    r.stop_after = 0;
    raw_stage(&r, &st, 1);
    info.stop_ctx = &r;
    r.stop_after = 1;
    CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_STOPPED);    // stopped as soon as it is asked
    CHECK(r.rows <= 32);

    // A stage that refuses the page.
    raw_stage(&r, &st, 1);
    r.refuse = 1;
    info.stop = NULL;
    CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_STAGE);
    CHECK_EQ_INT(r.rows, 0);
    CHECK_EQ_INT(r.ends, 0);

    // A model there is none of.
    raw_stage(&r, &st, 1);
    sw_tp_info_t none = info_for(SW_NONE, 0);
    CHECK_EQ_INT(sw_testpage(&none, &st), SW_TP_NOMEM);
    CHECK_EQ_INT(r.begins, 0);
    // No stage, or one with no ink rows for a color page.
    CHECK_EQ_INT(sw_testpage(&info, NULL), SW_TP_STAGE);
    raw_stage(&r, &st, 0);
    CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_STAGE);
    CHECK_EQ_INT(r.begins, 0);
}

// ---- memory: every buffer through bigmem.h, and not many of them

static void test_memory(void)
{
    for (int cart = 0; cart < 2; cart++) {
        sw_tp_info_t info = info_for(SW_C2500, cart);
        raw_t r;
        rip_stage_t st;
        raw_stage(&r, &st, cart);
        big_asked_bytes = big_asked_n = 0;
        CHECK_EQ_INT(sw_testpage(&info, &st), SW_TP_DONE);
        // About 100 KB of PSRAM over the page, the largest piece well under what the
        // photo's screens need; a page is never held.
        CHECK(big_asked_bytes > 8 * 1024);
        printf("    %s page asks bigmem for %zu bytes in %zu requests\n", cart ? "color" : "gray",
               big_asked_bytes, big_asked_n);
        CHECK(big_asked_bytes < 400 * 1024);
    }
}

int main(void)
{
    test_font();
    test_layout();
    test_margins();
    test_colour_page();
    test_gray_page(SW_C2500, 0);
    test_gray_page(SW_II, 0);
    test_stops();
    test_memory();
    free(g_buf);
    TEST_END;
}
