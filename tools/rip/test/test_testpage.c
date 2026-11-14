// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page every printer prints (testpage.c, W-1140; W-1103 first drew it for a
// StyleWriter). Every family's geometry is held to the page: it fits the ImageWriter's 9.5 in
// and its own sheet, and nothing is drawn off the layout. A StyleWriter's page is fed through the
// real page stage (sw_page.c), the frames it writes are decoded by a reader written here from the
// wire rules (sw_decode_row for the rows themselves), and the page that comes out is held to what
// the design says it has: a letterhead in the top inch, a type tester, a ruler with a tick every
// 36 dots (0.1 in), four squares of exactly 360 x 360 dots, six screens (Apple's among them),
// seven swatches and four ink ramps with a color cartridge and gray steps and a black ramp
// without. The type tester and the panels are held to references made here from the stored
// pictures, the resampler, the halftoner and the model's tone curve, so either drawn through the
// wrong screen fails. The stored pictures are held to what made them: tp_pictures.h to
// tp_scene.c (W-1133), tp_type.h to the sizes it says it has. The ImageWriter's page goes
// through its strips (iw_stage.h, held to the rip's own in test_ripjob_stage.c).
#include "test_util.h"
#include "testpage.h"
#include "tp_iw.h"
#include "iw_stage.h"
#include "sw_testpage.h"
#include "qd_testpage.h"
#include "lw_caps.h"
#include "sw_page.h"
#include "sw_code.h"
#include "tp_face.h"
#include "tp_pictures.h"
#include "tp_type.h"
#include "tp_icons.h"
#include "tp_unpack.h"
#include "tp_scene.h"
#include "resample.h"
#include "ink.h"
#include <stdlib.h>

size_t big_asked_bytes, big_asked_n;

// ---- the stored pictures

// tp_pictures.h is what tp_scene.c traces at the panel's size: a change to the scene, ink_gray
// or the sizes without `make -C tools/rip scene` fails here. Held within two levels rather than
// exactly, so a host whose maths rounds a little differently from the one that made it passes.
static void test_stored_scene(void)
{
    const uint32_t w = TP_PANEL_W, h = TP_PANEL_H;
    uint8_t *rgb = malloc((size_t)w * 3), *want = malloc(w), *got = malloc(w);
    tp_unpack_t *u = tp_unpack_new(tp_panel_z, sizeof tp_panel_z);
    CHECK(u != NULL);
    int worst = 0;
    size_t off = 0;
    for (uint32_t y = 0; y < h; y++) {
        tp_scene_row_aspect(w, h, y, rgb, (float)h / (float)w);
        ink_gray(rgb, w, want);
        CHECK_EQ_INT(tp_unpack_read(u, got, w), 0);      // the last row too, which ends the stream
        for (size_t i = 0; i < w; i++) {
            int dd = abs(want[i] - got[i]);
            off += dd != 0;
            if (dd > worst) worst = dd;
        }
    }
    CHECK(worst <= 2);
    CHECK(off <= (size_t)w * h / 1000);
    CHECK_EQ_INT(tp_unpack_read(u, got, 1), -1);          // nothing past the end
    tp_unpack_free(u);
    // On the host (zlib), a stream cut short gives its rows up to the cut, then fails rather
    // than making more up. The board's tinfl would read zeros on; its streams are whole.
    u = tp_unpack_new(tp_panel_z, sizeof tp_panel_z / 2);
    CHECK(u != NULL);
    uint32_t rows = 0;
    while (rows < h && tp_unpack_read(u, got, w) == 0) rows++;
    CHECK(rows > 0 && rows < h);
    tp_unpack_free(u);
    free(rgb);
    free(want);
    free(got);
}

// tp_type.h: TP_TYPE_H rows, each line's ink around its baseline, capitals about 0.72 em over
// it and the descenders of "quick" and "jumps" under it, in both columns and nowhere else.
static void test_stored_type(void)
{
    const uint32_t rb = (TP_TYPE_W + 7) / 8;
    uint8_t *img = malloc((size_t)rb * TP_TYPE_H);
    tp_unpack_t *u = tp_unpack_new(tp_type_z, sizeof tp_type_z);
    CHECK(u != NULL);
    for (uint32_t y = 0; y < TP_TYPE_H; y++) CHECK_EQ_INT(tp_unpack_read(u, img + (size_t)y * rb, rb), 0);
    CHECK_EQ_INT(tp_unpack_read(u, img, 1), -1);
    tp_unpack_free(u);
    uint32_t prev = 0, prev_em = 0;
    for (int n = 0; n < TP_TYPE_LINES; n++) {
        const uint32_t em = (uint32_t)tp_type_pt[n] * TP_TYPE_DPI / 72, base = tp_type_base[n];
        CHECK(base > prev && base < TP_TYPE_H);
        // The rows from under the last line's descenders to under this line's.
        const uint32_t y0 = n ? prev + prev_em * 3 / 10 : 0, y1 = base + em * 3 / 10;
        CHECK(y1 <= TP_TYPE_H);
        for (int c = 0; c < 2; c++) {
            // The column's sample, from its label's place plus the gap.
            const uint32_t x0 = tp_type_col_x[c] + TP_TYPE_SAMPLE_DX, x1 = c ? TP_TYPE_W : tp_type_col_x[1];
            uint32_t top = UINT32_MAX, bottom = 0, left = UINT32_MAX;
            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = tp_type_col_x[c]; x < x1; x++)
                    if (img[(size_t)y * rb + x / 8] >> (7 - x % 8) & 1) {
                        if (y < top) top = y;
                        if (y > bottom) bottom = y;
                        if (x < left) left = x;
                    }
            CHECK(left >= x0 && left < x0 + em / 4);     // nothing in the label's place
            CHECK(top + em * 6 / 10 < base && top + em > base);      // capitals about 0.72 em up
            CHECK(bottom > base + em / 10);
        }
        // Nothing between this line's descenders and the next line's capitals.
        prev = base;
        prev_em = em;
    }
    free(img);
}

// ---- the face

static void test_font(void)
{
    // Seven rows of five columns, a blank space and no other blank glyph, and no two the same.
    CHECK_EQ_INT(sizeof tp_face / sizeof tp_face[0], 95);
    for (int a = 0; a < 95; a++) {
        int ink = 0;
        for (int r = 0; r < 7; r++) {
            CHECK(tp_face[a][r] < 32);
            ink += __builtin_popcount(tp_face[a][r]);
        }
        CHECK(a == 0 ? ink == 0 : ink > 0);
        for (int b = a + 1; b < 95; b++) CHECK(memcmp(tp_face[a], tp_face[b], 7) != 0);
    }
    // Capitals and digits fill the seven rows; the lowercase letters that sit on the line leave
    // the top two rows free, and the ones that hang below it (g j p q y) reach the seventh.
    for (const char *c = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"; *c; c++) {
        const uint8_t *g = tp_face[*c - 0x20];
        CHECK(g[0] != 0);
        CHECK(g[6] != 0);
    }
    for (const char *c = "acemnorsuvwxz"; *c; c++) {
        const uint8_t *g = tp_face[*c - 0x20];
        CHECK(g[0] == 0 && g[1] == 0);
        CHECK(g[6] != 0);
    }
    for (const char *c = "gjpqy"; *c; c++) CHECK(tp_face[*c - 0x20][6] != 0);
}

// Every printer's icon, by the names AirPrint gives them (printers_core.c pr_icon), 32 x 32
// pixels of its own colours, paper first.
static void test_icons(void)
{
    static const char *const names[] = { "icon", "iwlq", "stylewriter", "sworiginal", "sw2", "sw1200",
                                         "csw1500", "csw2200", "plw300", "plwls", "lw", "lw2", "plw", "clw",
                                         "lwselect", "lwpro400", "lwpro600", "lwpro810", "lw4600",
                                         "lw8500", "lw12640" };
    CHECK_EQ_INT(sizeof tp_icons / sizeof tp_icons[0], sizeof names / sizeof names[0]);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const tp_icon_t *ic = NULL;
        for (size_t k = 0; k < sizeof tp_icons / sizeof tp_icons[0]; k++)
            if (!strcmp(tp_icons[k].name, names[i])) ic = &tp_icons[k];
        CHECK(ic != NULL);
        if (!ic) continue;
        CHECK(ic->n >= 2);
        CHECK(ic->rgb[0][0] == 255 && ic->rgb[0][1] == 255 && ic->rgb[0][2] == 255);
        uint32_t inked = 0;
        for (uint32_t p = 0; p < TP_ICON_S * TP_ICON_S; p++) {
            CHECK(ic->px[p] < ic->n);
            inked += ic->px[p] != 0;
        }
        CHECK(inked > 100);
    }
}

// ---- rows given to a stage, as the page writes them

typedef struct {
    int      begins, ends, rows, ink_rows, refuse;
    uint32_t w, h;
    uint32_t first_ink, last_ink;       // rows with ink, in sheet rows
    uint32_t ink_x0, ink_x1;            // the first and last dots with ink, across
    int      padding_ink;               // bits past the row's width
    int      stop_after;                // stop() answers yes after this many rows (0: never)
    int      stop_calls;
} raw_t;

static void raw_scan(raw_t *r, const uint8_t *const *planes, int n)
{
    uint32_t y = (uint32_t)r->rows++;
    int ink = 0;
    const uint32_t rb = (r->w + 7) / 8;
    for (int p = 0; p < n; p++) {
        if (!planes[p]) continue;
        for (uint32_t b = 0; b < rb; b++) {
            if (!planes[p][b]) continue;
            ink = 1;
            for (uint32_t k = 0; k < 8; k++)
                if (planes[p][b] >> (7 - k) & 1) {
                    if (b * 8 + k < r->ink_x0) r->ink_x0 = b * 8 + k;
                    if (b * 8 + k > r->ink_x1) r->ink_x1 = b * 8 + k;
                }
            if (b == rb - 1 && (r->w & 7) && (planes[p][b] & (0xFF >> (r->w & 7)))) r->padding_ink = 1;
        }
    }
    if (!ink) return;
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
    r->first_ink = r->ink_x0 = UINT32_MAX;
    memset(st, 0, sizeof *st);
    st->page_begin = raw_begin;
    st->row = raw_row;
    st->page_end = raw_end;
    st->ctx = r;
    if (ink) st->ink_row = raw_ink_row;
}

static tp_info_t info_for(const tp_geom_t *g)
{
    tp_info_t i = {
        .printer = "Den StyleWriter", .board = "v0.3.0 at 10.0.1.115",
        .settings = "http://airwriter-2434.local", .gain = 0, .screen = HT_ATKINSON, .geom = g,
    };
    return i;
}

// ---- every family's page fits, and draws nowhere else

typedef struct { const char *what; tp_geom_t g; } fam_t;

static int families(fam_t *f, int max)
{
    int n = 0;
    static const iw_ribbon_t ribbons[3] = { IW_RIBBON_COLOUR, IW_RIBBON_BLACK, IW_RIBBON_UNKNOWN };
    for (int r = 0; r < 3; r++) {
        f[n].what = "ImageWriter II";
        CHECK_EQ_INT(tp_iw_geom("ImageWriter II", "icon", ribbons[r], &f[n++].g), 0);
    }
    f[n].what = "ImageWriter LQ";
    CHECK_EQ_INT(tp_iw_geom("ImageWriter LQ", "iwlq", IW_RIBBON_COLOUR, &f[n++].g), 0);
    f[n].what = "ImageWriter";
    CHECK_EQ_INT(tp_iw_geom("ImageWriter", "icon", IW_RIBBON_COLOUR, &f[n++].g), 0);
    for (int m = SW_ORIGINAL; m < SW_MODELS; m++)
        for (int cart = 0; cart < 2; cart++) {
            f[n].what = sw_spec((sw_model_t)m)->name;
            CHECK_EQ_INT(sw_tp_geom((sw_model_t)m, cart, &f[n++].g), 0);
        }
    f[n].what = "Personal LaserWriter 300";
    CHECK_EQ_INT(qd_tp_geom(QD_PLW300, &f[n++].g), 0);
    static const lw_page_t pages[3] = { LW_LETTER, LW_A4, LW_LEGAL };
    for (int p = 0; p < 3; p++) {
        lw_info_t li;
        lw_info_default(&li);
        li.page = pages[p];
        if (pages[p] == LW_A4) { li.area[2] = 57528; li.area[3] = 82389; }
        if (pages[p] == LW_LEGAL) li.area[3] = 99000;
        f[n].what = "LaserWriter";
        lw_tp_geom(&li, "lw", &f[n++].g);
    }
    // A LaserWriter II NT as one answered: 0.18 in in from the sides, 0.2 in at the top.
    lw_info_t li;
    lw_info_default(&li);
    li.area[0] = 1296; li.area[2] = 59904; li.area[3] = 77760;
    f[n].what = "LaserWriter II NT";
    lw_tp_geom(&li, "lw2", &f[n++].g);
    CHECK(n <= max);
    return n;
}

static uint32_t sc(uint32_t dpi, uint32_t v) { return (v * dpi + 180) / 360; }

static void test_every_family(void)
{
    static fam_t f[40];
    const int n = families(f, 40);
    for (int k = 0; k < n; k++) {
        const tp_geom_t *g = &f[k].g;
        tp_info_t info = info_for(g);
        tp_layout_t l;
        tp_layout(&info, &l);
        const uint32_t lw = sc(g->dpi_x, TP_W_360);
        CHECK(l.last_ink > 0);                                   // a page that can be drawn
        CHECK(l.last_ink < sc(g->dpi_y, TP_H_360));              // within the ImageWriter's 9.5 in
        CHECK((uint32_t)g->top + l.last_ink < g->sheet_h - g->margin_b);
        CHECK((uint32_t)g->left + lw <= g->sheet_w);
        CHECK(l.letterhead.y + l.letterhead.h <= g->dpi_y);      // the letterhead in the top inch
        CHECK_EQ_INT(l.panels, g->apple ? 6 : 5);
        CHECK_EQ_INT(l.colour, g->colour);
        // Everything inside the layout's 8 in, and nothing sits on anything else.
        tp_box_t all[64];
        int m = 0;
        all[m++] = l.letterhead;
        all[m++] = l.details;
        all[m++] = l.type;
        all[m++] = l.ruler;
        all[m++] = l.footer;
        all[m++] = l.screen_names;
        for (int i = 0; i < 4; i++) all[m++] = l.square[i];
        for (int i = 0; i < l.panels; i++) { all[m++] = l.panel[i]; all[m++] = l.panel_ramp[i]; }
        if (l.colour) {
            for (int i = 0; i < TP_SWATCHES; i++) all[m++] = l.swatch[i];
            for (int i = 0; i < 4; i++) all[m++] = l.ink_ramp[i];
        } else {
            all[m++] = l.ink_ramp[3];
            for (int i = 0; i < 11; i++) all[m++] = l.gray_step[i];
        }
        for (int i = 0; i < m; i++) {
            CHECK(all[i].w > 0 && all[i].h > 0);
            CHECK(all[i].x + all[i].w <= lw);
            CHECK(all[i].y + all[i].h <= l.last_ink + 1);
            for (int j = i + 1; j < m; j++)
                CHECK(all[i].x + all[i].w <= all[j].x || all[j].x + all[j].w <= all[i].x ||
                      all[i].y + all[i].h <= all[j].y || all[j].y + all[j].h <= all[i].y);
        }
        // An inch each way, as near as the family's dots allow.
        for (int i = 0; i < 4; i++) {
            CHECK_EQ_INT(l.square[i].w, g->dpi_x);
            CHECK_EQ_INT(l.square[i].h, g->dpi_y);
        }
        // Each ramp's name is under it, where every label on the page goes.
        // The page through a stage: the whole sheet, its ink only on the layout's rows and dots.
        raw_t r;
        rip_stage_t st;
        raw_stage(&r, &st, g->colour);
        CHECK_EQ_INT(tp_page(&info, &st), TP_DONE);
        CHECK_EQ_INT(r.begins, 1);
        CHECK_EQ_INT(r.ends, 1);
        CHECK_EQ_INT(r.w, g->sheet_w);
        CHECK_EQ_INT(r.h, g->sheet_h);
        CHECK_EQ_INT((uint32_t)r.rows, g->sheet_h);
        // (The first row that may carry ink is the letterhead's; the icon's own paper is over its
        // first dots.)
        CHECK(r.first_ink >= g->top + l.first_ink && r.first_ink < g->top + l.first_ink + g->dpi_y / 10);
        CHECK_EQ_INT(r.last_ink, g->top + l.last_ink);
        CHECK(r.ink_x0 >= g->left && r.ink_x1 < g->left + lw);
        CHECK(!r.padding_ink);
    }
}

// The ImageWriter's model line and the note a gray page has; the StyleWriter's and the
// LaserWriter's.
static void test_model_lines(void)
{
    tp_geom_t g;
    tp_iw_geom("ImageWriter II", "icon", IW_RIBBON_COLOUR, &g);
    CHECK(!strcmp(g.model, "ImageWriter II, color ribbon") && g.colour && !g.gray_note);
    tp_iw_geom("ImageWriter II", "icon", IW_RIBBON_BLACK, &g);
    CHECK(!strcmp(g.model, "ImageWriter II, black ribbon") && !g.colour && g.gray_note);
    tp_iw_geom("ImageWriter LQ", "iwlq", IW_RIBBON_UNKNOWN, &g);
    CHECK(!strcmp(g.model, "ImageWriter LQ") && !g.colour && g.gray_note);
    tp_iw_geom("ImageWriter", "icon", IW_RIBBON_COLOUR, &g);           // the original: no color
    CHECK(!strcmp(g.model, "ImageWriter") && !g.colour && !g.gray_note);
    CHECK_EQ_INT(tp_iw_geom(NULL, "icon", IW_RIBBON_COLOUR, &g), -1);
    CHECK_EQ_INT(g.dpi_x, 144);
    CHECK_EQ_INT(g.dpi_y, 72);
    sw_tp_geom(SW_C2500, 0, &g);
    CHECK(!strcmp(g.model, "Color StyleWriter 2500, black cartridge") && g.gray_note && !g.colour);
    CHECK(!strcmp(g.icon, "stylewriter"));
    sw_tp_geom(SW_II, 1, &g);
    CHECK(!strcmp(g.model, "StyleWriter II") && !g.gray_note && !g.colour);
    CHECK(!strcmp(g.icon, "sw2"));
    CHECK_EQ_INT(sw_tp_geom(SW_NONE, 0, &g), -1);
    lw_info_t li;
    lw_info_default(&li);
    snprintf(li.product, sizeof li.product, "LaserWriter II NT");
    snprintf(li.version, sizeof li.version, "47.0");
    lw_tp_geom(&li, "lw2", &g);
    CHECK(!strcmp(g.model, "LaserWriter II NT, PostScript 47.0"));
    CHECK_EQ_INT(g.sheet_w, 2550);
    CHECK_EQ_INT(g.sheet_h, 3300);
    CHECK_EQ_INT(g.left, 75);                                    // centred on its quarter-inch area
    CHECK_EQ_INT(g.top, 75);
    CHECK_EQ_INT(g.apple, 0);
    CHECK(!strcmp(g.paper, "Letter"));
    // On A4 and Legal, the sheet the printer has, named on the page.
    li.page = LW_A4;
    lw_tp_geom(&li, "lw2", &g);
    CHECK(!strcmp(g.paper, "A4"));
    CHECK_EQ_INT(g.sheet_w, 2480);
    CHECK_EQ_INT(g.sheet_h, 3508);
    li.page = LW_LEGAL;
    lw_tp_geom(&li, "lw2", &g);
    CHECK(!strcmp(g.paper, "Legal"));
    CHECK_EQ_INT(g.sheet_h, 4200);
    // Every other family's page is Letter.
    tp_iw_geom("ImageWriter II", "icon", IW_RIBBON_COLOUR, &g);
    CHECK(g.paper == NULL);
    sw_tp_geom(SW_C2500, 1, &g);
    CHECK(g.paper == NULL);
}

// ---- a StyleWriter's frames from the real stage, decoded

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
    d->nrows = SW_TP_SHEET_H - s->margin_t - s->margin_b;
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

// Draws the page of `info` (a StyleWriter's geometry for m and cart) through the real stage and
// decodes it.
static int page(dec_t *d, const tp_info_t *info, sw_model_t m, int cart)
{
    g_n = 0;
    sw_page_cfg_t cfg = { .model = m, .colour_cart = cart, .out = sink };
    sw_page_t *p = sw_page_new(&cfg);
    CHECK(p != NULL);
    if (!p) return -1;
    rip_stage_t st;
    sw_page_stage(p, &st);
    int rc = tp_page(info, &st);
    sw_page_free(p);
    decode(d, m, sw_printable_w(m, SW_TP_SHEET_W));
    return rc;
}

// ---- looking at a decoded page

static int bit(const dec_t *d, int ink, uint32_t x, uint32_t y)
{
    if (x >= d->pw || y >= d->nrows) return 0;
    return d->img[ink][(size_t)y * d->rowbytes + (x >> 3)] >> (7 - (x & 7)) & 1;
}

static uint32_t ink_in(const dec_t *d, int ink, const tp_box_t *b)
{
    uint32_t n = 0;
    for (uint32_t y = b->y; y < (uint32_t)b->y + b->h; y++)
        for (uint32_t x = b->x; x < (uint32_t)b->x + b->w; x++) n += (uint32_t)bit(d, ink, x, y);
    return n;
}

// A column slice of a box, in tenths of its width.
static uint32_t slice_ink(const dec_t *d, int ink, const tp_box_t *b, int tenth)
{
    tp_box_t s = *b;
    s.x = (uint16_t)(b->x + b->w * (uint32_t)tenth / 10);
    s.w = (uint16_t)(b->w / 10);
    return ink_in(d, ink, &s);
}

// The face as the page draws it at 360 dpi: each pixel 4 x 4 dots, a cell of 6 pixels across.
static void draw_text(uint8_t *bm, uint32_t bw, const char *s)
{
    for (uint32_t i = 0; s[i]; i++) {
        const uint8_t *g = tp_face[s[i] - 0x20];
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
static int has_text(const dec_t *d, const tp_box_t *b, const char *s)
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
                    if (bm[(size_t)j * bw + i] != bit(d, INK_K, x + i, y + j)) ok = 0;
            found = ok;
        }
    free(bm);
    return found;
}

// The box of rows just under b, as tall as a line, where the page puts a label.
static tp_box_t under(const tp_box_t *b)
{
    return (tp_box_t){ b->x, (uint16_t)(b->y + b->h), (uint16_t)(b->w > 600 ? b->w : 600), 70 };
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
static int components_of_size(const dec_t *d, uint32_t w, uint32_t h, tp_box_t *out, int max)
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
            if (found < max) out[found] = (tp_box_t){ (uint16_t)x0, (uint16_t)y0, (uint16_t)w, (uint16_t)h };
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

// A halftoner as a StyleWriter's job makes it: its tone curve, black's place on the tile, and
// Apple's tile started a row on.
static void sw_ht(ht_t *h, ht_algo_t a, uint32_t w, int gain, sw_model_t m)
{
    CHECK_EQ_INT(ht_init(h, a, w), 0);
    ht_set_gain(h, gain);
    ht_set_tone(h, sw_tone(m, a));
    ink_screen_phase(h, IW_BLACK);
    if (a == HT_APPLE) ht_set_phase(h, h->phase_x, h->phase_y + 1, h->invert);
}

// ---- the type tester, reproduced here: the stored samples averaged down to 360 dpi as a
// phone's page is, through the screen in use; the size labels sit in the gap the samples leave.

typedef struct { uint8_t *rows; uint32_t w, made; } ref_t;

static void ref_row(void *ctx, const uint8_t *row, uint32_t w)
{
    ref_t *r = ctx;
    memcpy(r->rows + (size_t)r->made++ * w, row, w);
}

static uint32_t type_misses(const dec_t *d, const tp_box_t *b, ht_algo_t a, int gain, sw_model_t m)
{
    ref_t ref = { .rows = malloc((size_t)b->w * b->h), .w = b->w };
    rs_t rs;
    CHECK_EQ_INT(rs_init(&rs, TP_TYPE_W, TP_TYPE_H, b->w, b->h, ref_row, &ref), 0);
    const uint32_t rb = (TP_TYPE_W + 7) / 8;
    uint8_t *src = malloc(rb), *gs = malloc(TP_TYPE_W);
    tp_unpack_t *u = tp_unpack_new(tp_type_z, sizeof tp_type_z);
    for (uint32_t y = 0; y < TP_TYPE_H; y++) {
        CHECK_EQ_INT(tp_unpack_read(u, src, rb), 0);
        for (uint32_t x = 0; x < TP_TYPE_W; x++) gs[x] = src[x >> 3] >> (7 - (x & 7)) & 1 ? 0 : 255;
        rs_push(&rs, gs);
    }
    tp_unpack_free(u);
    rs_free(&rs);
    CHECK_EQ_INT(ref.made, b->h);
    ht_t h;
    sw_ht(&h, a, b->w, gain, m);
    uint8_t *bits = calloc((b->w + 7) / 8, 1);
    uint32_t bad = 0;
    // The labels' columns (a fifth of an inch of each column's half inch) are the face's.
    const uint32_t lab0 = tp_type_col_x[0] / 2, lab1 = tp_type_col_x[1] / 2, labw = TP_TYPE_SAMPLE_DX / 2;
    for (uint32_t y = 0; y < b->h; y++) {
        ht_row(&h, ref.rows + (size_t)y * b->w, bits);
        for (uint32_t x = 0; x < b->w; x++) {
            if ((x >= lab0 && x < lab0 + labw) || (x >= lab1 && x < lab1 + labw)) continue;
            bad += (uint32_t)((bits[x >> 3] >> (7 - (x & 7)) & 1) != bit(d, INK_K, b->x + x, b->y + y));
        }
    }
    ht_free(&h);
    free(bits);
    free(src);
    free(gs);
    free(ref.rows);
    return bad;
}

// ---- a StyleWriter's color page, decoded

static void test_colour_page(void)
{
    tp_geom_t g;
    sw_tp_geom(SW_C2500, 1, &g);
    tp_info_t info = info_for(&g);
    info.gain = 7;
    info.screen = HT_FLOYD;
    dec_t d;
    CHECK_EQ_INT(page(&d, &info, SW_C2500, 1), TP_DONE);
    tp_layout_t l;
    tp_layout(&info, &l);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.pages, 1);
    CHECK_EQ_INT(d.page_ends, 1);
    CHECK_EQ_INT(d.colour, 1);
    CHECK(d.rect_kind_c > 3 && d.rect_kind_r == 0);
    CHECK_EQ_INT(d.nrows, 3816);
    CHECK_EQ_INT(d.pw, 2916);

    // The letterhead is in the top inch, in all four inks (the icon) and black (the words).
    CHECK(l.letterhead.y + l.letterhead.h <= 360);
    for (int k = 0; k < 4; k++) CHECK(ink_in(&d, k, &l.icon) > 200);
    tp_box_t right = { (uint16_t)(l.letterhead.x + l.letterhead.w / 2), l.letterhead.y,
                       (uint16_t)(l.letterhead.w / 2), l.letterhead.h };
    CHECK(ink_in(&d, INK_K, &right) > 1000);                     // "Test page" and the title

    // The board's details in the face, at 4 x 4 dots a pixel.
    CHECK(has_text(&d, &l.details, "Color StyleWriter 2500, color cartridge"));
    CHECK(has_text(&d, &l.details, "Den StyleWriter"));
    CHECK(has_text(&d, &l.details, "AirWriter"));
    CHECK(has_text(&d, &l.details, "v0.3.0 at 10.0.1.115"));
    CHECK(has_text(&d, &l.details, "http://airwriter-2434.local"));
    CHECK(has_text(&d, &l.details, "Floyd-Steinberg, dot gain 7%"));
    CHECK(has_text(&d, &l.details, "Printer"));
    CHECK(has_text(&d, &l.details, "Letter, 2916 x 3816 at 360 dpi"));    // 3060 - 72 - 72, 3960 - 45 - 99
    CHECK(has_text(&d, &l.details, "72 left, 45 top, 99 bottom dots"));
    CHECK(!has_text(&d, &l.details, "No such thing"));

    // The type tester: the samples as a phone's page draws them, through the screen in use, in
    // black alone; each size labelled; every line with ink in both columns.
    CHECK(l.type.y > l.details.y + l.details.h);
    CHECK_EQ_INT(l.type.w, 2880);
    CHECK_EQ_INT(l.type.h, (TP_TYPE_H * 360 + 360) / 720);
    CHECK_EQ_INT(type_misses(&d, &l.type, HT_FLOYD, 7, SW_C2500), 0);
    for (int k = 0; k < 3; k++) CHECK_EQ_INT(ink_in(&d, k, &l.type), 0);
    {
        tp_box_t heads = { 0, (uint16_t)(l.type.y - 60), 2880, 60 };
        CHECK(has_text(&d, &heads, "Sans serif"));
        CHECK(has_text(&d, &heads, "Serif"));
    }
    for (int n = 0; n < TP_TYPE_LINES; n++) {
        char lab[8];
        snprintf(lab, sizeof lab, "%u pt", (unsigned)tp_type_pt[n]);
        const tp_box_t *b = &l.type_line[n];
        tp_box_t left = { 0, b->y, 1440, (uint16_t)(b->h + 4) }, right2 = { 1476, b->y, 1404, (uint16_t)(b->h + 4) };
        CHECK(has_text(&d, &left, lab));
        CHECK(has_text(&d, &right2, lab));
        tp_box_t s0 = { 180, b->y, 1260, b->h }, s1 = { 1656, b->y, 1224, b->h };
        CHECK(ink_in(&d, INK_K, &s0) > 100);
        CHECK(ink_in(&d, INK_K, &s1) > 100);
    }

    // Seven solid swatches in one row across the page: each ink alone, then red (magenta over
    // yellow), green (cyan over yellow) and blue (cyan over magenta), each solid in its inks and
    // clear of the others, with its name under it (W-1134).
    static const int inks[TP_SWATCHES] = {
        1 << INK_C, 1 << INK_M, 1 << INK_Y, 1 << INK_K,
        1 << INK_M | 1 << INK_Y, 1 << INK_C | 1 << INK_Y, 1 << INK_C | 1 << INK_M,
    };
    static const char *const names[TP_SWATCHES] = { "Cyan", "Magenta", "Yellow", "Black",
                                                    "Red", "Green", "Blue" };
    for (int i = 0; i < TP_SWATCHES; i++) {
        tp_box_t b = l.swatch[i];
        uint32_t area = (uint32_t)b.w * b.h;
        CHECK(area > 0);
        for (int k = 0; k < 4; k++)
            CHECK_EQ_INT(ink_in(&d, k, &b), inks[i] >> k & 1 ? area : 0);
        CHECK_EQ_INT(b.y, l.swatch[0].y);
        CHECK_EQ_INT(b.w, l.swatch[0].w);
        if (i) CHECK(b.x > l.swatch[i - 1].x + l.swatch[i - 1].w);
        tp_box_t name = under(&b);
        CHECK(has_text(&d, &name, names[i]));
    }
    CHECK_EQ_INT(l.swatch[0].x, 0);
    CHECK_EQ_INT(l.swatch[TP_SWATCHES - 1].x + l.swatch[TP_SWATCHES - 1].w, 2880);
    CHECK(l.swatch[0].y > l.type.y + l.type.h);

    // A ramp of each ink from nothing to all of it, two by two, each named under it.
    for (int i = 0; i < 4; i++) {
        const tp_box_t *b = &l.ink_ramp[i];
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
        tp_box_t name = under(b);
        CHECK(has_text(&d, &name, names[i]));
        CHECK(b->y > l.swatch[0].y + l.swatch[0].h);
    }
    CHECK_EQ_INT(l.ink_ramp[0].y, l.ink_ramp[1].y);
    CHECK_EQ_INT(l.ink_ramp[2].y, l.ink_ramp[3].y);
    CHECK(l.ink_ramp[2].y > l.ink_ramp[0].y + l.ink_ramp[0].h + 28);
    CHECK_EQ_INT(l.ink_ramp[1].x + l.ink_ramp[1].w, 2880);

    // The six screens, each reproduced here from the halftoner, the still life, the model's
    // tone curve and the dot gain: a panel through another screen is not the same dots. The
    // drivers start Apple's tile on its second row, and black sits where it does.
    ht_t ph[6], rh[6];
    tp_box_t pb = l.panel[0], rb = l.panel_ramp[0];
    int apple = -1;
    CHECK_EQ_INT(l.panels, 6);
    for (int p = 0; p < 6; p++) {
        ht_algo_t a = l.panel_screen[p];
        if (a == HT_APPLE) apple = p;
        CHECK_EQ_INT(l.panel[p].w, pb.w);
        CHECK_EQ_INT(l.panel[p].h, pb.h);
        CHECK_EQ_INT(l.panel_ramp[p].w, rb.w);
        sw_ht(&ph[p], a, pb.w, info.gain, SW_C2500);
        sw_ht(&rh[p], a, rb.w, info.gain, SW_C2500);
    }
    CHECK(apple >= 0);
    {
        uint8_t *gr = malloc(pb.w), *bits = calloc((pb.w + 7) / 8, 1);
        uint32_t bad[6] = { 0 };
        CHECK_EQ_INT(pb.w, TP_PANEL_W);
        CHECK_EQ_INT(pb.h, TP_PANEL_H);
        tp_unpack_t *z = tp_unpack_new(tp_panel_z, sizeof tp_panel_z);
        CHECK(z != NULL);
        for (uint32_t y = 0; y < pb.h; y++) {
            CHECK_EQ_INT(tp_unpack_read(z, gr, pb.w), 0);
            for (int p = 0; p < 6; p++) {
                ht_row(&ph[p], gr, bits);
                for (uint32_t x = 0; x < pb.w; x++)
                    bad[p] += ((bits[x >> 3] >> (7 - (x & 7)) & 1) != bit(&d, INK_K, l.panel[p].x + x, l.panel[p].y + y));
            }
        }
        tp_unpack_free(z);
        for (int p = 0; p < 6; p++) CHECK_EQ_INT(bad[p], 0);
        // Each ramp runs 255 to 0 across.
        for (uint32_t x = 0; x < rb.w; x++) gr[x] = (uint8_t)(255.0 - 255.0 * x / (rb.w - 1));
        uint32_t rbad[6] = { 0 };
        for (uint32_t y = 0; y < rb.h; y++)
            for (int p = 0; p < 6; p++) {
                ht_row(&rh[p], gr, bits);
                for (uint32_t x = 0; x < rb.w; x++)
                    rbad[p] += ((bits[x >> 3] >> (7 - (x & 7)) & 1) != bit(&d, INK_K, l.panel_ramp[p].x + x, l.panel_ramp[p].y + y));
            }
        for (int p = 0; p < 6; p++) CHECK_EQ_INT(rbad[p], 0);
        free(gr);
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
        tp_box_t one = { l.panel[p].x, l.screen_names.y, 480, l.screen_names.h };
        CHECK_EQ_INT(has_text(&d, &one, screen_name(l.panel_screen[p])), !on);
    }

    // The ruler: an edge, and a tick every 36 dots (0.1 in), the half inch longer and the inch
    // longer than that.
    {
        const tp_box_t *r = &l.ruler;
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
        tp_box_t nums = { 0, (uint16_t)(r->y + r->h), 2880, 60 };
        CHECK(has_text(&d, &nums, "0"));
        CHECK(has_text(&d, &nums, "4"));
        CHECK(has_text(&d, &nums, "8"));
    }

    // Four squares, exactly 360 x 360 dots each, an inch, found in the ink itself.
    tp_box_t sq[8];
    CHECK_EQ_INT(components_of_size(&d, 360, 360, sq, 8), 4);
    for (int i = 0; i < 4; i++) {
        int hit = 0;
        for (int j = 0; j < 4; j++) hit |= sq[j].x == l.square[i].x && sq[j].y == l.square[i].y;
        CHECK(hit);
    }
    {
        const tp_box_t *s = l.square;
        CHECK_EQ_INT(ink_in(&d, INK_K, &s[1]), 360 * 360);                          // solid black
        uint32_t checker = ink_in(&d, INK_K, &s[3]);
        CHECK(checker > 360 * 360 / 2 - 400 && checker < 360 * 360 / 2 + 400);      // the finest checker
        uint32_t grid = ink_in(&d, INK_K, &s[0]);
        CHECK(grid > 1000 && grid < 360 * 360 / 4);
        uint32_t lines = ink_in(&d, INK_K, &s[2]);
        CHECK(lines > 360 * 360 / 5 && lines < 360 * 360 / 2);
        for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) CHECK_EQ_INT(ink_in(&d, k, &s[i]), 0);
        tp_box_t cap = under(&s[1]);
        cap.w = 700;
        cap.h = 120;
        CHECK(has_text(&d, &cap, "White line: a nozzle is out"));
    }

    // Nothing is drawn below the footer or past 8 in across.
    CHECK_EQ_INT(ink_outside(&d, l.last_ink), 0);
    CHECK(has_text(&d, &l.footer, "wells.ee"));
    CHECK(has_text(&d, &l.footer, "github.com/wr/airwriter"));
    dec_free(&d);
}

static void test_gray_page(sw_model_t m, int cart)
{
    tp_geom_t g;
    sw_tp_geom(m, cart, &g);
    tp_info_t info = info_for(&g);
    info.screen = HT_APPLE;
    dec_t d;
    CHECK_EQ_INT(page(&d, &info, m, cart), TP_DONE);
    tp_layout_t l;
    tp_layout(&info, &l);
    const sw_spec_t *s = sw_spec(m);
    CHECK_EQ_INT(d.bad, 0);
    CHECK_EQ_INT(d.page_ends, 1);
    CHECK_EQ_INT(d.colour, 0);
    CHECK(d.rect_kind_r > 3 && d.rect_kind_c == 0);                // black rects: one ink
    CHECK_EQ_INT(d.nrows, SW_TP_SHEET_H - s->margin_t - s->margin_b);

    // Black alone on the whole page; the ink planes are never touched.
    for (int k = 0; k < 3; k++) {
        uint32_t n = 0;
        for (size_t i = 0; i < (size_t)d.nrows * d.rowbytes; i++) n += d.img[k][i];
        CHECK_EQ_INT(n, 0);
    }
    CHECK(has_text(&d, &l.details, s->colour ? "black cartridge" : s->name));
    CHECK(has_text(&d, &l.details, s->name));
    // The margins and printable page are the family's, written out here from the design's table.
    CHECK(has_text(&d, &l.details, m == SW_II ? "Letter, 2880 x 3774 at 360 dpi"
                                              : "Letter, 2916 x 3816 at 360 dpi"));
    CHECK(has_text(&d, &l.details, m == SW_II ? "72 left, 72 top, 114 bottom dots"
                                              : "72 left, 45 top, 99 bottom dots"));
    CHECK_EQ_INT(ink_outside(&d, l.last_ink), 0);
    CHECK_EQ_INT(type_misses(&d, &l.type, HT_APPLE, 0, m), 0);
    // A gray ramp with black, empty at one end and full at the other, named under it.
    const tp_box_t *b = &l.ink_ramp[3];
    uint32_t tenth = (uint32_t)b->w / 10 * b->h;
    CHECK(slice_ink(&d, INK_K, b, 0) < tenth / 10);
    CHECK(slice_ink(&d, INK_K, b, 9) > tenth * 8 / 10);
    tp_box_t name = under(b);
    CHECK(has_text(&d, &name, "Black"));
    // Eleven steps, 0 to 100 %, each darker than the one before it, each labelled under it.
    uint32_t last = 0;
    for (int j = 0; j < 11; j++) {
        uint32_t v = ink_in(&d, INK_K, &l.gray_step[j]);
        if (j == 0) CHECK_EQ_INT(v, 0);
        else CHECK(v > last);
        if (j == 10) CHECK_EQ_INT(v, (uint32_t)l.gray_step[j].w * l.gray_step[j].h);
        last = v;
        char pc[8];
        snprintf(pc, sizeof pc, "%d%%", j * 10);
        tp_box_t lab = under(&l.gray_step[j]);
        lab.w = 140;
        CHECK(has_text(&d, &lab, pc));
    }
    CHECK(l.gray_step[10].x + l.gray_step[10].w < b->x);
    // A gray page on a printer that prints in color says why.
    if (s->colour) {
        tp_box_t note = { 0, (uint16_t)(l.gray_step[0].y - 80), 2880, 80 };
        CHECK(has_text(&d, &note, "The black cartridge is in."));
    }
    // The same ruler and squares.
    tp_box_t sq[8];
    CHECK_EQ_INT(components_of_size(&d, 360, 360, sq, 8), 4);
    for (uint32_t k = 0; k < 80; k++) CHECK(bit(&d, INK_K, k * 36, l.ruler.y + 12));
    dec_free(&d);
}

// ---- the ImageWriter's page, through its strips

typedef struct { uint8_t *b; size_t n, cap; } grow_t;

static void grow(void *ctx, const uint8_t *b, size_t n)
{
    grow_t *g = ctx;
    if (g->n + n > g->cap) { g->cap = (g->n + n) * 2; g->b = realloc(g->b, g->cap); }
    memcpy(g->b + g->n, b, n);
    g->n += n;
}

static int has(const grow_t *g, const char *s, size_t n)
{
    for (size_t i = 0; i + n <= g->n; i++)
        if (!memcmp(g->b + i, s, n)) return 1;
    return 0;
}

static void test_imagewriter_page(void)
{
    for (int colour = 0; colour < 2; colour++) {
        tp_geom_t g;
        tp_iw_geom("ImageWriter II", "icon", colour ? IW_RIBBON_COLOUR : IW_RIBBON_BLACK, &g);
        tp_info_t info = info_for(&g);
        grow_t s = { 0 };
        iw_stage_t ist;
        iw_stage_init(&ist, grow, &s, g.colour, -144);
        rip_stage_t st;
        iw_stage(&ist, &st);
        CHECK_EQ_INT(tp_page(&info, &st), TP_DONE);
        iw_stage_free(&ist);
        // ESC c and the strips' set-up first, the form feed last, the inks named on the color
        // ribbon alone, and never ESC F (docs/traps.md).
        CHECK(s.n > 20000 && s.n < 150000);
        CHECK(!memcmp(s.b, "\x1b" "c" "\x1b" "p" "\x1b" "T16", 8));
        CHECK_EQ_INT(s.b[s.n - 1], 0x0C);
        CHECK_EQ_INT(has(&s, "\x1b" "K1", 3), colour);
        CHECK_EQ_INT(has(&s, "\x1b" "K3", 3), colour);
        printf("    the ImageWriter's %s page: %zu bytes\n", colour ? "color" : "gray", s.n);
        free(s.b);
    }
}

// ---- how it ends

static void test_stops(void)
{
    tp_geom_t g;
    sw_tp_geom(SW_C2500, 1, &g);
    tp_info_t info = info_for(&g);
    raw_t r;
    rip_stage_t st;
    raw_stage(&r, &st, 1);
    info.stop = raw_stop;
    info.stop_ctx = &r;
    r.stop_after = 500;
    CHECK_EQ_INT(tp_page(&info, &st), TP_STOPPED);
    CHECK(r.rows >= 500 && r.rows < 540);                    // asked every 32 rows
    CHECK_EQ_INT(r.begins, 1);
    CHECK_EQ_INT(r.ends, 0);                                 // no page end follows a stop
    raw_stage(&r, &st, 1);
    info.stop_ctx = &r;
    r.stop_after = 1;
    CHECK_EQ_INT(tp_page(&info, &st), TP_STOPPED);           // stopped as soon as it is asked
    CHECK(r.rows <= 32);
    // A page stopped before it starts sends nothing: no page begins.
    raw_stage(&r, &st, 1);
    info.stop_ctx = &r;
    r.stop_after = -1;
    CHECK_EQ_INT(tp_page(&info, &st), TP_STOPPED);
    CHECK_EQ_INT(r.begins, 0);
    CHECK_EQ_INT(r.rows, 0);

    // A stage that refuses the page.
    raw_stage(&r, &st, 1);
    r.refuse = 1;
    info.stop = NULL;
    CHECK_EQ_INT(tp_page(&info, &st), TP_STAGE);
    CHECK_EQ_INT(r.rows, 0);
    CHECK_EQ_INT(r.ends, 0);

    // No geometry, or one the page cannot be drawn at.
    raw_stage(&r, &st, 1);
    tp_info_t none = info_for(NULL);
    CHECK_EQ_INT(tp_page(&none, &st), TP_NOMEM);
    tp_geom_t odd = g;
    odd.dpi_x = odd.dpi_y = 600;
    none = info_for(&odd);
    CHECK_EQ_INT(tp_page(&none, &st), TP_NOMEM);
    odd = g;
    odd.sheet_w = 2000;                                      // narrower than the layout
    none = info_for(&odd);
    CHECK_EQ_INT(tp_page(&none, &st), TP_NOMEM);
    CHECK_EQ_INT(r.begins, 0);
    tp_layout_t l;
    tp_layout(&none, &l);
    CHECK_EQ_INT(l.last_ink, 0);
    // No stage, or one with no ink rows for a color page.
    CHECK_EQ_INT(tp_page(&info, NULL), TP_STAGE);
    raw_stage(&r, &st, 0);
    CHECK_EQ_INT(tp_page(&info, &st), TP_STAGE);
    CHECK_EQ_INT(r.begins, 0);
}

// ---- memory: every buffer through bigmem.h, and not many of them

static void test_memory(void)
{
    static fam_t f[40];
    const int n = families(f, 40);
    for (int k = 0; k < n; k++) {
        tp_info_t info = info_for(&f[k].g);
        raw_t r;
        rip_stage_t st;
        raw_stage(&r, &st, f[k].g.colour);
        big_asked_bytes = big_asked_n = 0;
        CHECK_EQ_INT(tp_page(&info, &st), TP_DONE);
        // A page is never held: a few parts' halftoners and readers at a time.
        CHECK(big_asked_bytes > 8 * 1024);
        CHECK(big_asked_bytes < 400 * 1024);
        if (k == 0 || k == n - 1)
            printf("    %s %s page asks bigmem for %zu bytes in %zu requests\n", f[k].what,
                   f[k].g.colour ? "color" : "gray", big_asked_bytes, big_asked_n);
    }
}

int main(void)
{
    test_stored_scene();
    test_stored_type();
    test_font();
    test_icons();
    test_model_lines();
    test_every_family();
    test_colour_page();
    test_gray_page(SW_C2500, 0);
    test_gray_page(SW_II, 0);
    test_imagewriter_page();
    test_stops();
    test_memory();
    free(g_buf);
    TEST_END;
}
