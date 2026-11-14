// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page every printer prints (testpage.h). The page is a list of parts, each a run of
// rows with a function that fills one row of it, and a list of lines of text the face draws over
// them. Parts that screen a picture or a ramp make their halftoners when their first row comes
// and free them after their last, so only the parts on a row are held. All sizes are in dots at
// 360 dpi from the layout's top-left, scaled to the family's dots across (SX, S8) and down (SY)
// where they are used; `plan` lays the page out once and the same plan serves the page and the
// layout the tests read.
//
// Everything that grows is through bigmem.h; a row of text, a rule, the ruler and the squares
// are drawn into the row's planes a dot at a time and cost nothing but time.
#include "testpage.h"
#include "tp_face.h"
#include "tp_words.h"
#include "tp_icons.h"
#include "tp_pictures.h"
#include "tp_type.h"
#include "tp_unpack.h"
#include "resample.h"
#include "ink.h"
#include "bigmem.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W        TP_W_360           // the layout is 8 in across
#define PX       4                  // dots to a pixel of the face at 360 dpi (6 a cell, 7 a glyph)
#define LINE     60                 // from one line of text to the next
#define TOP      36                 // clear paper over the letterhead
#define GAP      90                 // between one part of the page and the next

// The pictures and ramps, all multiples of 8 across where they are drawn so a halftoned row is
// copied in by the byte.
#define PANEL_W  400
#define PANEL_H  300
#define PANEL_DX 496                // six panels; five spread over the same 8 in
#define PRAMP_H  48
#define STEP_W   128
#define STEP_GAP 8
#define STEPS_H  200
#define RAMP_W   1408               // the four ink ramps, two by two
#define RAMP_H   80
#define RAMP_DY  160
#define BLACK_X  1600               // a gray page's black ramp, beside the steps
#define ICON_S   6                  // dots to an icon pixel at 360 dpi
#define ICON_MAX (TP_ICON_S * ICON_S)

// The picture is traced once on the host (tools/rip/mkscene.c, W-1133), at the panel's size.
_Static_assert(TP_PANEL_W == PANEL_W && TP_PANEL_H == PANEL_H, "tp_pictures.h: make -C tools/rip scene");

enum { MAX_TEXT = 96, MAX_SECT = 40, POOL = 4096 };

typedef struct tp tp_t;
typedef struct sect sect_t;
typedef void (*fill_fn)(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS]);

// A run of rows. `x` and `w` (multiples of 8) are the columns of a picture; `ink` and `idx`
// are for the part that needs them.
struct sect {
    uint32_t y0, rows, x, w;
    int      ink, idx;
    int    (*begin)(tp_t *t, sect_t *s);    // made when its first row is wanted: 0, or -1 for no memory
    fill_fn  fill;                           // y counts from its first row
    void   (*end)(tp_t *t, sect_t *s);
    void    *st;
    enum { PART_NEW, PART_HELD, PART_DONE } state;     // its begin has run / its end has
};

typedef struct { uint16_t x, y; uint8_t n, bold; const char *s; } text_t;

struct tp {
    const tp_info_t    *info;
    const tp_geom_t    *geom;
    const tp_words_t   *words;
    const tp_icon_t    *icon;
    int                 colour;
    ht_algo_t           screen;             // the record's, HT_APPLE for the family's Apple
    int                 gain;
    // The page's dots and the sizes it scales once.
    uint32_t            dx, dy, w, px_x, px_y, cell, gh, line;
    uint32_t            icon_sx, icon_sy, icon_w, icon_h, title_x, title_y, sub_y, head_h;
    uint32_t            panel_w, panel_h, panel_dx, step_w, step_dx;
    tp_layout_t         lay;
    text_t              txt[MAX_TEXT];
    int                 ntxt;
    char                pool[POOL];
    size_t              pool_n;
    sect_t              sec[MAX_SECT];
    int                 nsec;
    uint32_t            lo, hi;              // the first and last rows with ink
    int                 full;                // the plan outgrew a table, or the page
};

// ------------------------------------------------------------------ sizes

// A size in dots at 360 dpi at the page's dots, rounded: across, down, and across down to a whole
// byte for a picture's columns. SX1 and SY1 are never less than a dot, for lines.
static uint32_t scale(uint32_t dpi, uint32_t v) { return (v * dpi + 180) / 360; }
static uint32_t SX(const tp_t *t, uint32_t v) { return scale(t->dx, v); }
static uint32_t SY(const tp_t *t, uint32_t v) { return scale(t->dy, v); }
static uint32_t S8(const tp_t *t, uint32_t v) { return SX(t, v) & ~7u; }
static uint32_t SX1(const tp_t *t, uint32_t v) { uint32_t s = SX(t, v); return s ? s : 1; }
static uint32_t SY1(const tp_t *t, uint32_t v) { uint32_t s = SY(t, v); return s ? s : 1; }

// ------------------------------------------------------------------ bits

static void setb(uint8_t *row, uint32_t x) { row[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); }

static void span(uint8_t *row, uint32_t x, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) setb(row, x + i);
}

// w bits of src (MSB first) into row from x0, any x0.
static void blit(uint8_t *row, uint32_t x0, const uint8_t *src, uint32_t w)
{
    for (uint32_t x = 0; x < w; x++)
        if (src[x >> 3] & (0x80 >> (x & 7))) setb(row, x0 + x);
}

// The same for a picture row: x and w multiples of 8.
static void put_bits(uint8_t *row, uint32_t x, const uint8_t *src, uint32_t w)
{
    uint8_t *d = row + x / 8;
    for (uint32_t i = 0; i < w / 8; i++) d[i] |= src[i];
}

// n bytes of a layout row into a sheet row `left` dots in, any left.
static void shift_in(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t left)
{
    const uint32_t o = left >> 3, s = left & 7;
    for (uint32_t i = 0; i < n; i++) {
        dst[o + i] |= (uint8_t)(src[i] >> s);
        if (s) dst[o + i + 1] |= (uint8_t)(src[i] << (8 - s));
    }
}

// ------------------------------------------------------------------ the face

// The check mark that ticks the screen in use is the face's one glyph past 0x7E: text never
// takes it from outside, `tick` draws it.
#define TICK 0x7F
static const uint8_t k_tick[7] = { 0x00, 0x01, 0x01, 0x02, 0x12, 0x0C, 0x04 };
static const char k_tick_text[2] = { TICK, '\0' };

static const uint8_t *glyph(unsigned char c)
{
    if (c == TICK) return k_tick;
    return tp_face[(c >= 0x20 && c < 0x7F ? c : '?') - 0x20];
}

// A line of text at (x, y): the sanitised string kept in the pool. Printable ASCII is kept; a
// byte outside it is one '?' (the continuation bytes of UTF-8 dropped). At most `cap`
// characters, and never past the layout's right edge. Bold is a dot wider in every pixel.
static void text(tp_t *t, uint32_t x, uint32_t y, const char *s, size_t cap, int bold)
{
    if (!s) s = "";
    const size_t room = x < t->w ? (t->w - x - 1) / t->cell : 0;
    if (cap > room) cap = room;
    if (t->ntxt >= MAX_TEXT || t->pool_n + cap > POOL) { t->full = 1; return; }
    char *d = t->pool + t->pool_n;
    size_t k = 0;
    for (; *s && k < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c >= 0x80 && c < 0xC0) continue;
        d[k++] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    t->pool_n += k;
    t->txt[t->ntxt++] = (text_t){ (uint16_t)x, (uint16_t)y, (uint8_t)k, (uint8_t)bold, d };
    if (y < t->lo) t->lo = y;
    if (y + t->gh - 1 > t->hi) t->hi = y + t->gh - 1;
}

static void tick(tp_t *t, uint32_t x, uint32_t y, int bold)
{
    if (t->ntxt >= MAX_TEXT || x + t->cell >= t->w) { t->full = 1; return; }
    t->txt[t->ntxt++] = (text_t){ (uint16_t)x, (uint16_t)y, 1, (uint8_t)bold, k_tick_text };
}

static void text_row(const tp_t *t, uint32_t y, uint8_t *k)
{
    for (int i = 0; i < t->ntxt; i++) {
        const text_t *it = &t->txt[i];
        if (y < it->y || y >= it->y + t->gh) continue;
        uint32_t py = (y - it->y) / t->px_y;
        for (uint32_t c = 0; c < it->n; c++) {
            uint8_t g = glyph((unsigned char)it->s[c])[py];
            for (uint32_t col = 0; col < 5; col++)
                if (g >> (4 - col) & 1)
                    span(k, it->x + c * t->cell + col * t->px_x, t->px_x + (uint32_t)it->bold);
        }
    }
}

// ------------------------------------------------------------------ halftoners

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

// The screens, in the order the page shows them; Apple's only for a family that has its own.
static const ht_algo_t k_screens[TP_PANELS] = { HT_ATKINSON, HT_FLOYD, HT_BLUENOISE, HT_BAYER,
                                                HT_THRESHOLD, HT_APPLE };

// A halftoner as the family's jobs make it for one ink: the screen (its own Apple screen for
// HT_APPLE), the dot gain, its tone curve for that screen, and, where its jobs place them, the
// ink's place on the tile with Apple's tile started on its second row (ripjob.c's
// apple_page_start). 0, or -1 for no memory.
static int ht_make(const tp_t *t, ht_t *h, ht_algo_t algo, uint32_t w, iw_ink_t ink)
{
    const tp_geom_t *g = t->geom;
    const ht_algo_t a = algo == HT_APPLE ? g->apple_algo : algo;
    if (ht_init(h, a, w)) return -1;
    ht_set_gain(h, t->gain);
    if (g->tone) ht_set_tone(h, g->tone(g->tone_ctx, a));
    if (g->ink_place) {
        ink_screen_phase(h, ink);
        if (a == HT_APPLE) ht_set_phase(h, h->phase_x, h->phase_y + 1, h->invert);
    }
    return 0;
}

// ------------------------------------------------------------------ parts: rules, swatches

static void fill_rule(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)y;
    memset(bits[IW_BLACK] + s->x / 8, 0xFF, s->w / 8);
}

// A swatch is solid in each ink of its mask (`idx`, a bit per iw_ink_t): one ink, or two
// printed over each other for red, green and blue.
static void fill_swatch(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)y;
    for (int k = 0; k < IW_INKS; k++)
        if (s->idx >> k & 1) memset(bits[k] + s->x / 8, 0xFF, s->w / 8);
}

// ------------------------------------------------------------------ part: the letterhead

typedef struct {
    ink_screens_t inks;                     // the icon, a color page
    ht_t          gray;                     // and a gray one
    uint8_t       rgb[ICON_MAX * 3], g[ICON_MAX], ib[IW_INKS][ICON_MAX / 8];
} head_t;

#define TITLE_DX 72                     // from the icon to the title
#define TITLE_Y  56
#define SUB_Y    106

static int begin_head(tp_t *t, sect_t *s)
{
    head_t *h = big_calloc(1, sizeof *h);
    if (!h) return -1;
    // The icon is pixel art: the ordered screen keeps its edges, in the inks it is drawn in.
    if (t->colour ? ink_screens_init(&h->inks, HT_BAYER, 0, t->icon_w)
                  : ht_init(&h->gray, HT_BAYER, t->icon_w)) {
        free(h);
        return -1;
    }
    s->st = h;
    return 0;
}

static void end_head(tp_t *t, sect_t *s)
{
    head_t *h = s->st;
    if (t->colour) ink_screens_free(&h->inks); else ht_free(&h->gray);
    free(h);
}

static void fill_head(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    head_t *h = s->st;
    const tp_icon_t *ic = t->icon;
    const tp_words_t *w = t->words;
    if (y < t->icon_h) {
        const uint8_t *row = ic->px + (size_t)(y / t->icon_sy) * TP_ICON_S;
        for (uint32_t x = 0; x < t->icon_w; x++) {
            uint8_t k = row[x / t->icon_sx];
            memcpy(h->rgb + x * 3, ic->rgb[k < ic->n ? k : 0], 3);
        }
        if (t->colour) {
            uint8_t *const ib[IW_INKS] = { h->ib[0], h->ib[1], h->ib[2], h->ib[3] };
            ink_screens_row(&h->inks, h->rgb, ib);
            for (int i = 0; i < IW_INKS; i++) put_bits(bits[i], 0, h->ib[i], t->icon_w);
        } else {
            uint8_t b[ICON_MAX / 8];
            ink_gray(h->rgb, t->icon_w, h->g);
            ht_row(&h->gray, h->g, b);
            put_bits(bits[IW_BLACK], 0, b, t->icon_w);
        }
    }
    if (y >= t->title_y && y < t->title_y + w->title_h)
        blit(bits[IW_BLACK], t->title_x, w->title + (size_t)(y - t->title_y) * ((w->title_w + 7u) / 8), w->title_w);
    if (y >= t->sub_y && y < t->sub_y + w->sub_h)
        blit(bits[IW_BLACK], t->w - w->sub_w, w->sub + (size_t)(y - t->sub_y) * ((w->sub_w + 7u) / 8), w->sub_w);
}

// ------------------------------------------------------------------ part: a ramp of one ink

typedef struct { ht_t h; uint8_t g[W], b[W / 8]; } ramp_t;

static int begin_ramp(tp_t *t, sect_t *s)
{
    ramp_t *r = big_calloc(1, sizeof *r);
    if (!r) return -1;
    if (ht_make(t, &r->h, t->screen, s->w, (iw_ink_t)s->ink)) { free(r); return -1; }
    for (uint32_t x = 0; x < s->w; x++) r->g[x] = (uint8_t)(255.0 - 255.0 * x / (s->w - 1));
    s->st = r;
    return 0;
}

static void end_ramp(tp_t *t, sect_t *s)
{
    (void)t;
    ramp_t *r = s->st;
    ht_free(&r->h);
    free(r);
}

static void fill_ramp(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)y;
    ramp_t *r = s->st;
    ht_row(&r->h, r->g, r->b);
    put_bits(bits[s->ink], s->x, r->b, s->w);
}

// ------------------------------------------------------------------ part: eleven gray steps

typedef struct { ht_t h[11]; uint8_t g[STEP_W], b[STEP_W / 8]; } steps_t;

static int begin_steps(tp_t *t, sect_t *s)
{
    steps_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    for (int j = 0; j < 11; j++)
        if (ht_make(t, &p->h[j], t->screen, s->w, IW_BLACK)) {
            while (j--) ht_free(&p->h[j]);
            free(p);
            return -1;
        }
    s->st = p;
    return 0;
}

static void end_steps(tp_t *t, sect_t *s)
{
    (void)t;
    steps_t *p = s->st;
    for (int j = 0; j < 11; j++) ht_free(&p->h[j]);
    free(p);
}

static void fill_steps(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)y;
    steps_t *p = s->st;
    for (int j = 0; j < 11; j++) {
        memset(p->g, 255 - 255 * j / 10, s->w);
        ht_row(&p->h[j], p->g, p->b);
        put_bits(bits[IW_BLACK], s->x + (uint32_t)j * t->step_dx, p->b, s->w);
    }
}

// ------------------------------------------------------------------ part: the type tester

// The samples (tp_type.h, 1-bit at 720 dpi) averaged down to the page's dots as a phone's page
// is (resample.h), so the letters' edges come out gray as a phone draws them, then screened with
// the screen in use, in black.
typedef struct {
    tp_unpack_t *z;
    rs_t         rs;
    ht_t         h;
    uint32_t     made;                  // rows the resampler has given
    uint8_t      src[(TP_TYPE_W + 7) / 8], gs[TP_TYPE_W], g[W], b[W / 8];
} type_t;

static void type_row(void *ctx, const uint8_t *row, uint32_t w)
{
    type_t *p = ctx;
    memcpy(p->g, row, w);
    p->made++;
}

static int begin_type(tp_t *t, sect_t *s)
{
    type_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    if (!(p->z = tp_unpack_new(tp_type_z, sizeof tp_type_z))) { free(p); return -1; }
    if (rs_init(&p->rs, TP_TYPE_W, TP_TYPE_H, s->w, s->rows, type_row, p)) {
        tp_unpack_free(p->z);
        free(p);
        return -1;
    }
    if (ht_make(t, &p->h, t->screen, s->w, IW_BLACK)) {
        rs_free(&p->rs);
        tp_unpack_free(p->z);
        free(p);
        return -1;
    }
    s->st = p;
    return 0;
}

static void end_type(tp_t *t, sect_t *s)
{
    (void)t;
    type_t *p = s->st;
    ht_free(&p->h);
    rs_free(&p->rs);
    tp_unpack_free(p->z);
    free(p);
}

// The resampler only ever shrinks here (no page is past 720 dpi), so each row pushed gives at
// most one: push until row y has come. The stream is ours and held to mktype.py by the tests: a
// row it cannot give is paper.
static void fill_type(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t;
    type_t *p = s->st;
    while (p->made <= y && p->rs.src_y < TP_TYPE_H) {
        if (tp_unpack_read(p->z, p->src, sizeof p->src)) memset(p->src, 0, sizeof p->src);
        for (uint32_t x = 0; x < TP_TYPE_W; x++) p->gs[x] = p->src[x >> 3] & (0x80 >> (x & 7)) ? 0 : 255;
        rs_push(&p->rs, p->gs);
    }
    ht_row(&p->h, p->g, p->b);
    put_bits(bits[IW_BLACK], s->x, p->b, s->w);
}

// ------------------------------------------------------------------ part: the screens

// idx 0 is the panels themselves, which read the stored scene in gray; idx 1 the ramps under them.
typedef struct {
    ht_t         h[TP_PANELS];
    tp_unpack_t *z;
    uint32_t     zrows;                 // the scene's rows read
    uint8_t      src[TP_PANEL_W], g[PANEL_W], b[PANEL_W / 8];
} panels_t;

static int begin_panels(tp_t *t, sect_t *s)
{
    panels_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    if (s->idx == 0 && !(p->z = tp_unpack_new(tp_panel_z, sizeof tp_panel_z))) {
        free(p);
        return -1;
    }
    for (int i = 0; i < t->lay.panels; i++)
        if (ht_make(t, &p->h[i], k_screens[i], t->panel_w, IW_BLACK)) {
            while (i--) ht_free(&p->h[i]);
            tp_unpack_free(p->z);
            free(p);
            return -1;
        }
    s->st = p;
    return 0;
}

static void end_panels(tp_t *t, sect_t *s)
{
    panels_t *p = s->st;
    for (int i = 0; i < t->lay.panels; i++) ht_free(&p->h[i]);
    tp_unpack_free(p->z);
    free(p);
}

// The still life through each screen, in black alone, and under it a ramp. The stored scene is
// the panel's size at 360 dpi; at other dots its rows and columns are picked to the panel's.
static void fill_panels(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    panels_t *p = s->st;
    const uint32_t pw = t->panel_w;
    if (s->idx == 0) {
        const uint32_t sy = y * TP_PANEL_H / s->rows;
        while (p->zrows <= sy) {
            if (tp_unpack_read(p->z, p->src, sizeof p->src)) memset(p->src, 255, sizeof p->src);
            p->zrows++;
        }
        for (uint32_t x = 0; x < pw; x++) p->g[x] = p->src[x * TP_PANEL_W / pw];
    } else {
        for (uint32_t x = 0; x < pw; x++) p->g[x] = (uint8_t)(255.0 - 255.0 * x / (pw - 1));
    }
    for (int i = 0; i < t->lay.panels; i++) {
        ht_row(&p->h[i], p->g, p->b);
        put_bits(bits[IW_BLACK], (uint32_t)i * t->panel_dx, p->b, pw);
    }
}

// ------------------------------------------------------------------ parts: ruler, squares

#define RULER_H    64
#define RULER_EDGE 4
#define TICK_TENTH 16
#define TICK_HALF  32
#define TICK_INCH  56

// 8 in with a tick every 36 dots, 0.1 in: the half inch longer, the inch longer still. The edge
// is four rows thick at 360 dpi, so that one dead nozzle thins it and cannot break it.
static void fill_ruler(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)s;
    uint8_t *k = bits[IW_BLACK];
    if (y < SY1(t, RULER_EDGE)) { memset(k, 0xFF, t->w / 8); return; }
    uint32_t d = y - SY1(t, RULER_EDGE), tw = SX1(t, 2);
    for (uint32_t i = 0; i <= 80; i++) {
        uint32_t len = SY(t, i % 10 == 0 ? TICK_INCH : i % 5 == 0 ? TICK_HALF : TICK_TENTH);
        if (d < len) span(k, i < 80 ? SX(t, i * 36) : t->w - tw, tw);   // (the last one inside the 8 in)
    }
}

#define SQ 360

// Four squares an inch each way, 2 in apart: a quarter-inch grid, solid black, vertical lines
// every 8 dots at 360 dpi and the finest checker. Each fills its inch corner to corner.
static void fill_squares(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    uint8_t *k = bits[IW_BLACK];
    const uint32_t sq = SX(t, SQ), sqy = s->rows, tx = SX1(t, 2), ty = SY1(t, 2);
    const uint32_t qx = SX(t, 90), qy = SY(t, 90), pitch = SX1(t, 8);
    // 1: the grid
    if (y % qy < ty || y >= sqy - ty) {
        span(k, 0, sq);
    } else {
        for (uint32_t x = 0; x < sq; x += qx) span(k, x, tx);
        span(k, sq - tx, tx);
    }
    // 2: solid
    span(k, SX(t, 720), sq);
    // 3: lines, framed
    const uint32_t lx = SX(t, 1440);
    if (y < ty || y >= sqy - ty) {
        span(k, lx, sq);
    } else {
        for (uint32_t x = 0; x < sq; x += pitch) span(k, lx + x, tx);
        span(k, lx + sq - tx, tx);
    }
    // 4: the checker
    const uint32_t cx = SX(t, 2160);
    for (uint32_t x = 0; x < sq; x++)
        if ((x + y) % 2 == 0) setb(k, cx + x);
}

// ------------------------------------------------------------------ the plan

static tp_box_t bx(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return (tp_box_t){ (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
}

static void push(tp_t *t, sect_t s)
{
    if (t->nsec >= MAX_SECT || s.x + s.w > t->w) { t->full = 1; return; }
    t->sec[t->nsec++] = s;
    if (s.y0 < t->lo) t->lo = s.y0;
    if (s.y0 + s.rows - 1 > t->hi) t->hi = s.y0 + s.rows - 1;
}

static void rule(tp_t *t, uint32_t y)
{
    push(t, (sect_t){ .y0 = y, .rows = SY1(t, 3), .x = 0, .w = t->w, .fill = fill_rule });
}

// A heading: bold, with a rule under it. The part under it starts 64 dots on.
static uint32_t heading(tp_t *t, uint32_t y, const char *s)
{
    text(t, 0, y, s, 60, 1);
    rule(t, y + SY(t, 40));
    return y + SY(t, 64);
}

static void textf(tp_t *t, uint32_t x, uint32_t y, size_t cap, int bold, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
static void textf(tp_t *t, uint32_t x, uint32_t y, size_t cap, int bold, const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    text(t, x, y, b, cap, bold);
}

static const char *const k_ink_name[4] = { "Cyan", "Magenta", "Yellow", "Black" };
static const iw_ink_t k_ink_of[4] = { IW_CYAN, IW_MAGENTA, IW_YELLOW, IW_BLACK };

// The color row: the four inks, then the three colors two of them make printed over each other
// (W-1134).
#define INK(k) (1 << (k))
static const char *const k_swatch_name[TP_SWATCHES] = { "Cyan", "Magenta", "Yellow", "Black",
                                                        "Red", "Green", "Blue" };
static const int k_swatch_inks[TP_SWATCHES] = {
    INK(IW_CYAN), INK(IW_MAGENTA), INK(IW_YELLOW), INK(IW_BLACK),
    INK(IW_MAGENTA) | INK(IW_YELLOW), INK(IW_CYAN) | INK(IW_YELLOW), INK(IW_CYAN) | INK(IW_MAGENTA),
};
#define SWATCH_W  336               // nearly an inch; the seven span the 8 in
#define SWATCH_H  150

// Under the four squares: what each is, and what a good print of it looks like. The ImageWriter's;
// a family with other words gives its own (tp_geom_t.captions).
static const char *const k_captions[2][4] = {
    { "One inch each way", "Solid black", "Straight lines", "Even gray" },
    { "Measure with a ruler", "White line: a dead pin", "The head is aligned", "The paper feeds evenly" },
};

static void plan(tp_t *t)
{
    const tp_geom_t *g = t->geom;
    const tp_info_t *i = t->info;
    tp_layout_t *L = &t->lay;
    const uint32_t W_ = t->w, CELL_ = t->cell, GH_ = t->gh, LINE_ = t->line, GAP_ = SY(t, GAP);
    uint32_t y = SY(t, TOP);
    t->lo = UINT32_MAX;
    t->hi = 0;

    // The letterhead, and a rule.
    L->letterhead = bx(0, y, W_, t->head_h);
    L->icon = bx(0, y, t->icon_w, t->icon_h);
    push(t, (sect_t){ .y0 = y, .rows = t->head_h, .begin = begin_head, .fill = fill_head, .end = end_head });
    y += t->head_h + SY(t, 16);
    rule(t, y);
    y += SY1(t, 3) + SY(t, 56);

    // The board, in two columns: labels, then the values, cut to their column.
    {
        char page[80], margins[80];
        const char *paper = g->paper ? g->paper : "Letter";
        if (g->dpi_x == g->dpi_y)
            snprintf(page, sizeof page, "%s, %u x %u at %u dpi", paper, (unsigned)g->print_w,
                     (unsigned)g->print_h, (unsigned)g->dpi_x);
        else
            snprintf(page, sizeof page, "%s, %u x %u at %u x %u dpi", paper, (unsigned)g->print_w,
                     (unsigned)g->print_h, (unsigned)g->dpi_x, (unsigned)g->dpi_y);
        snprintf(margins, sizeof margins, "%u left, %u top, %u bottom dots", (unsigned)g->margin_l,
                 (unsigned)g->margin_t, (unsigned)g->margin_b);
        const uint32_t lx = 0, lv = 10 * CELL_, rx = SX(t, 1440), rv = rx + 10 * CELL_;
        const size_t cl = (rx - lv) / CELL_ - 1, cr = (W_ - rv) / CELL_;
        L->details = bx(0, y, W_, 3 * LINE_ + GH_);
        text(t, lx, y, "Printer", 9, 0);              text(t, lv, y, g->model, cl, 0);
        text(t, rx, y, "AirWriter", 9, 0);            text(t, rv, y, i->board, cr, 0);
        y += LINE_;
        text(t, lx, y, "Name", 9, 0);                 text(t, lv, y, i->printer, cl, 0);
        text(t, rx, y, "Halftone", 9, 0);
        if (t->gain > 0) textf(t, rv, y, cr, 0, "%s, dot gain %d%%", screen_name(t->screen), t->gain);
        else             text(t, rv, y, screen_name(t->screen), cr, 0);
        y += LINE_;
        text(t, lx, y, "Settings", 9, 0);             text(t, lv, y, i->settings, cl, 0);
        text(t, rx, y, "Page", 9, 0);                 text(t, rv, y, page, cr, 0);
        y += LINE_;
        text(t, rx, y, "Margins", 9, 0);              text(t, rv, y, margins, cr, 0);
        y += LINE_ + SY(t, GAP - 20);
    }

    // Type: a sans and a serif at five sizes, labelled, through the screen in use.
    {
        y = heading(t, y, "Type");
        text(t, SX(t, tp_type_col_x[0] / 2u), y, "Sans serif", 20, 0);
        text(t, SX(t, tp_type_col_x[1] / 2u), y, "Serif", 20, 0);
        y += LINE_;
        const uint32_t th = (TP_TYPE_H * t->dy + TP_TYPE_DPI / 2) / TP_TYPE_DPI;
        push(t, (sect_t){ .y0 = y, .rows = th, .x = 0, .w = W_, .begin = begin_type, .fill = fill_type,
                          .end = end_type });
        L->type = bx(0, y, W_, th);
        uint32_t prev = y;
        for (int n = 0; n < TP_TYPE_LINES; n++) {
            const uint32_t base = y + (tp_type_base[n] * t->dy + TP_TYPE_DPI / 2) / TP_TYPE_DPI;
            L->type_line[n] = bx(0, prev, W_, base - prev);
            for (int c = 0; c < 2; c++)
                textf(t, SX(t, tp_type_col_x[c] / 2u), base - GH_, 6, 0, "%u pt", (unsigned)tp_type_pt[n]);
            prev = base;
        }
        y += th + GAP_;
    }

    // The inks. In color: seven solid swatches (the four inks and the three they make in pairs),
    // then a ramp of each ink, two by two. In gray: eleven steps and a ramp of black side by side.
    // Every label under what it names.
    if (t->colour) {
        y = heading(t, y, "Color");
        const uint32_t sw = S8(t, SWATCH_W), sh = SY(t, SWATCH_H);
        for (int n = 0; n < TP_SWATCHES; n++) {
            const uint32_t x = ((W_ - sw) * (uint32_t)n / (TP_SWATCHES - 1)) & ~7u;
            push(t, (sect_t){ .y0 = y, .rows = sh, .x = x, .w = sw, .idx = k_swatch_inks[n],
                              .fill = fill_swatch });
            L->swatch[n] = bx(x, y, sw, sh);
            text(t, x, y + sh + SY(t, 16), k_swatch_name[n], 20, 0);
        }
        y += sh + SY(t, 16) + GH_ + SY(t, 64);
        const uint32_t rw = S8(t, RAMP_W), rh = SY(t, RAMP_H);
        for (int n = 0; n < 4; n++) {
            const uint32_t rx = n & 1 ? W_ - rw : 0, ry = y + SY(t, (uint32_t)(n / 2) * RAMP_DY);
            push(t, (sect_t){ .y0 = ry, .rows = rh, .x = rx, .w = rw, .ink = k_ink_of[n],
                              .begin = begin_ramp, .fill = fill_ramp, .end = end_ramp });
            L->ink_ramp[n] = bx(rx, ry, rw, rh);
            text(t, rx, ry + rh + SY(t, 16), k_ink_name[n], 20, 0);
        }
        y += SY(t, RAMP_DY) + rh + SY(t, 16) + GH_ + GAP_;
    } else {
        y = heading(t, y, "Gray");
        if (g->gray_note) {
            text(t, 0, y, g->gray_note, 100, 0);
            y += LINE_ + SY(t, 12);
        }
        const uint32_t sh = SY(t, STEPS_H);
        push(t, (sect_t){ .y0 = y, .rows = sh, .x = 0, .w = t->step_w,
                          .begin = begin_steps, .fill = fill_steps, .end = end_steps });
        for (int j = 0; j < 11; j++) {
            const uint32_t x = (uint32_t)j * t->step_dx;
            L->gray_step[j] = bx(x, y, t->step_w, sh);
            textf(t, x, y + sh + SY(t, 16), 5, 0, "%d%%", j * 10);
        }
        const uint32_t bx0 = S8(t, BLACK_X);
        push(t, (sect_t){ .y0 = y, .rows = sh, .x = bx0, .w = W_ - bx0, .ink = IW_BLACK,
                          .begin = begin_ramp, .fill = fill_ramp, .end = end_ramp });
        L->ink_ramp[3] = bx(bx0, y, W_ - bx0, sh);
        text(t, bx0, y + sh + SY(t, 16), "Black", 20, 0);
        y += sh + SY(t, 16) + GH_ + GAP_;
    }

    // The screens side by side, the one in use ticked.
    y = heading(t, y, "Halftone");
    L->screen_names = bx(0, y, W_, GH_);
    L->ticked = -1;
    const uint32_t py = y + SY(t, 44), ry = py + t->panel_h + SY(t, 12), rh = SY(t, PRAMP_H);
    for (int n = 0; n < L->panels; n++) {
        const uint32_t x = (uint32_t)n * t->panel_dx;
        const int on = k_screens[n] == t->screen;
        const char *name = screen_name(k_screens[n]);
        text(t, x, y, name, 18, on);
        if (on) {
            tick(t, x + ((uint32_t)strlen(name) + 1) * CELL_, y, 1);
            L->ticked = n;
        }
        L->panel_screen[n] = k_screens[n] == HT_APPLE ? g->apple_algo : k_screens[n];
        L->panel[n] = bx(x, py, t->panel_w, t->panel_h);
        L->panel_ramp[n] = bx(x, ry, t->panel_w, rh);
    }
    push(t, (sect_t){ .y0 = py, .rows = t->panel_h, .idx = 0, .begin = begin_panels, .fill = fill_panels, .end = end_panels });
    push(t, (sect_t){ .y0 = ry, .rows = rh, .idx = 1, .begin = begin_panels, .fill = fill_panels, .end = end_panels });
    y = ry + rh + SY(t, 32);
    text(t, 0, y, "The ticked screen prints your pages. Change it in the printer's settings.", 100, 0);
    y += GH_ + GAP_;

    // Alignment: a ruler 8 in long, numbered, and four squares an inch each way with captions.
    y = heading(t, y, "Alignment");
    const uint32_t rul_h = SY(t, RULER_H);
    push(t, (sect_t){ .y0 = y, .rows = rul_h, .fill = fill_ruler });
    L->ruler = bx(0, y, W_, rul_h);
    for (uint32_t n = 0; n <= 8; n++) {
        char d[2] = { (char)('0' + n), '\0' };
        text(t, n < 8 ? SX(t, n * 360) : W_ - CELL_ - 1, y + rul_h + SY(t, 12), d, 1, 0);
    }
    y += rul_h + SY(t, 12) + GH_ + SY(t, 72);
    const uint32_t sqx = SX(t, SQ), sqy = SY(t, SQ);
    push(t, (sect_t){ .y0 = y, .rows = sqy, .fill = fill_squares });
    for (int n = 0; n < 4; n++) L->square[n] = bx(SX(t, (uint32_t)n * 720), y, sqx, sqy);
    y += sqy + SY(t, 28);
    const char *const (*cap)[4] = g->captions ? g->captions : k_captions;
    for (int r = 0; r < 2; r++, y += LINE_)
        for (int n = 0; n < 4; n++)
            text(t, SX(t, (uint32_t)n * 720), y, cap[r][n], (SX(t, 720) - CELL_) / CELL_, 0);
    y += SY(t, GAP - LINE + 20);

    // The footer.
    rule(t, y);
    y += SY1(t, 3) + SY(t, 24);
    L->footer = bx(0, y, W_, GH_);
    text(t, 0, y, "AirWriter by Wells Workshop    wells.ee    github.com/wr/airwriter", 100, 0);

    L->first_ink = (uint16_t)t->lo;
    L->last_ink = (uint16_t)t->hi;
    L->colour = t->colour;
    // The page fits the ImageWriter's sheet on every printer, and its own.
    if (t->hi >= SY(t, TP_H_360) || g->top + t->hi >= g->sheet_h) t->full = 1;
}

// The page's sizes at its dots, then its plan. A geometry this page cannot draw (dots past 360,
// which every buffer is sized for, a grid the letterhead has no words at, or a sheet narrower
// than the layout) leaves the plan full: no page.
static void setup(tp_t *t, const tp_info_t *i)
{
    const tp_geom_t *g = i->geom;
    t->info = i;
    t->geom = g;
    t->colour = g->colour;
    t->screen = i->screen == HT_APPLE_LW ? HT_APPLE : i->screen;
    t->gain = i->gain;
    t->dx = g->dpi_x;
    t->dy = g->dpi_y;
    if (t->dx < 72 || t->dx > 360 || t->dy < 72 || t->dy > 360) { t->full = 1; return; }
    for (size_t n = 0; n < sizeof tp_words / sizeof tp_words[0]; n++)
        if (tp_words[n].dpi_x == t->dx && tp_words[n].dpi_y == t->dy) t->words = &tp_words[n];
    t->icon = &tp_icons[0];                         // the ImageWriter's, "icon"
    for (size_t n = 0; g->icon && n < sizeof tp_icons / sizeof tp_icons[0]; n++)
        if (!strcmp(tp_icons[n].name, g->icon)) t->icon = &tp_icons[n];
    t->w = SX(t, W);
    if (!t->words || g->left + t->w > g->sheet_w || t->w % 8) { t->full = 1; return; }
    t->px_x = SX1(t, PX);
    t->px_y = SY1(t, PX);
    t->cell = 6 * t->px_x;
    t->gh = 7 * t->px_y;
    t->line = SY(t, LINE);
    t->icon_sx = SX1(t, ICON_S);
    t->icon_sy = SY1(t, ICON_S);
    t->icon_w = TP_ICON_S * t->icon_sx;
    t->icon_h = TP_ICON_S * t->icon_sy;
    if (t->icon_w > ICON_MAX || t->icon_w % 8 || t->words->sub_w > t->w) { t->full = 1; return; }
    t->title_x = t->icon_w + SX(t, TITLE_DX);
    t->title_y = SY(t, TITLE_Y);
    t->sub_y = SY(t, SUB_Y);
    t->head_h = t->sub_y + t->words->sub_h;
    if (t->head_h < t->icon_h) t->head_h = t->icon_h;
    t->lay.panels = g->apple ? TP_PANELS : TP_PANELS - 1;
    t->panel_w = S8(t, PANEL_W);
    t->panel_h = SY(t, PANEL_H);
    t->panel_dx = g->apple ? S8(t, PANEL_DX) : ((t->w - t->panel_w) / (TP_PANELS - 2)) & ~7u;
    t->step_w = S8(t, STEP_W);
    t->step_dx = t->step_w + (S8(t, STEP_GAP) ? S8(t, STEP_GAP) : 8);
    plan(t);
}

void tp_layout(const tp_info_t *i, tp_layout_t *out)
{
    memset(out, 0, sizeof *out);
    if (!i || !i->geom) return;
    tp_t *t = big_calloc(1, sizeof *t);
    if (!t) return;
    setup(t, i);
    if (!t->full) *out = t->lay;
    free(t);
}

// ------------------------------------------------------------------ the page

int tp_page(const tp_info_t *i, const rip_stage_t *st)
{
    const tp_geom_t *g = i ? i->geom : NULL;
    if (!g) return TP_NOMEM;
    if (!st || !st->page_begin || !st->row || !st->page_end || (g->colour && !st->ink_row))
        return TP_STAGE;                     // a stage that cannot take this page's rows
    // A row of the sheet a byte longer than it needs, for a layout put in off the byte; and
    // with the layout away from the sheet's left, its own row to be drawn into first.
    const uint32_t rb = ((uint32_t)g->sheet_w + 7) / 8 + 1, lb = TP_W_360 / 8;

    tp_t *t = big_calloc(1, sizeof *t);
    uint8_t *rows = big_calloc(IW_INKS, rb), *lrows = g->left ? big_calloc(IW_INKS, lb) : NULL;
    if (!t || !rows || (g->left && !lrows)) {
        free(t);
        free(rows);
        free(lrows);
        return TP_NOMEM;
    }
    setup(t, i);
    uint8_t *out[IW_INKS], *planes[IW_INKS];
    for (int k = 0; k < IW_INKS; k++) {
        out[k] = rows + (size_t)k * rb;
        planes[k] = lrows ? lrows + (size_t)k * lb : out[k];
    }
    int rc = t->full ? TP_NOMEM : TP_DONE;

    // Asked before the page's first byte too: a page stopped before it starts sends nothing.
    if (rc == TP_DONE && i->stop && i->stop(i->stop_ctx)) rc = TP_STOPPED;
    if (rc == TP_DONE && st->page_begin(st->ctx, 0, g->sheet_w, g->sheet_h)) rc = TP_STAGE;
    for (uint32_t sy = 0; rc == TP_DONE && sy < g->sheet_h; sy++) {
        if ((sy & 31) == 0 && i->stop && i->stop(i->stop_ctx)) { rc = TP_STOPPED; break; }
        memset(rows, 0, (size_t)IW_INKS * rb);
        if (lrows) memset(lrows, 0, (size_t)IW_INKS * lb);
        // The layout starts `top` rows down the sheet; the rest are blank rows.
        if (sy >= g->top && sy - g->top <= t->hi) {
            const uint32_t y = sy - g->top;
            for (int n = 0; n < t->nsec && rc == TP_DONE; n++) {
                sect_t *s = &t->sec[n];
                if (y < s->y0 || y >= s->y0 + s->rows) continue;
                if (s->state == PART_NEW) {
                    if (s->begin && s->begin(t, s)) { rc = TP_NOMEM; break; }
                    s->state = PART_HELD;
                }
                s->fill(t, s, y - s->y0, planes);
                if (y == s->y0 + s->rows - 1) {
                    if (s->end) s->end(t, s);
                    s->state = PART_DONE;
                }
            }
            text_row(t, y, planes[IW_BLACK]);
            if (lrows)
                for (int k = 0; k < IW_INKS; k++) shift_in(out[k], planes[k], t->w / 8, g->left);
        }
        if (rc != TP_DONE) break;
        if (g->colour) st->ink_row(st->ctx, (const uint8_t *const *)out);
        else           st->row(st->ctx, out[IW_BLACK]);
    }
    if (rc == TP_DONE && st->page_end(st->ctx)) rc = TP_STAGE;

    // Whatever a part still holds (a page stopped or short of memory partway).
    for (int n = 0; n < t->nsec; n++)
        if (t->sec[n].state == PART_HELD && t->sec[n].end) t->sec[n].end(t, &t->sec[n]);
    free(lrows);
    free(rows);
    free(t);
    return rc;
}
