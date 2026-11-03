// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter's test page (sw_testpage.h). The page is a list of parts, each a run of rows
// with a function that fills one row of it, and a list of lines of text the face draws over
// them: the same idea as the ImageWriter's bands (testpage.c's fill_fn), at 360 dpi and square
// dots, one row at a time rather than in strips. Parts that screen a picture or a ramp make
// their halftoners when their first row comes and free them after their last, so only the
// parts on a row are held. All sizes are in dots at 360 dpi from the printable area's
// top-left; `plan` lays the page out once and the same plan serves the page and the layout
// the tests read.
//
// Everything that grows is through bigmem.h; a row of text, a rule, the ruler and the squares
// are drawn into the row's planes a dot at a time and cost nothing but time.
#include "sw_testpage.h"
#include "sw_font.h"
#include "sw_art.h"
#include "tp_scene.h"
#include "ink.h"
#include "bigmem.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define W        SW_TP_W            // the layout is 8 in across
#define PX       4                  // dots to a pixel of the face
#define CELL     (6 * PX)           // a character cell: five columns and a gap
#define GH       (7 * PX)           // a glyph
#define LINE     60                 // from one line of text to the next
#define TOP      48                 // clear paper over the letterhead
#define GAP      110                // between one part of the page and the next

// Where the pictures are, all multiples of 8 across so a halftoned row is copied in by the byte.
#define PHOTO_W  1264
#define PHOTO_H  700
#define SCENE_W  (PHOTO_W / 2)      // the still life is traced at 180 dpi and the screen does the rest
#define SCENE_H  (PHOTO_H / 2)
#define SIDE_X   1384               // the ramps and steps beside it
#define SIDE_W   (W - SIDE_X)       // 1496
#define PANEL_W  400
#define PANEL_H  300
#define PANEL_DX 496
#define PRAMP_H  48
#define STEP_W   128
#define STEP_DX  136
#define ICON_S   6
#define ICON_W   (SW_ICON_W * ICON_S)

enum { MAX_TEXT = 72, MAX_SECT = 40, POOL = 3072 };

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
    const sw_tp_info_t *info;
    sw_model_t          model;
    int                 colour;
    ht_algo_t           screen;
    int                 gain;
    sw_tp_layout_t      lay;
    text_t              txt[MAX_TEXT];
    int                 ntxt;
    char                pool[POOL];
    size_t              pool_n;
    sect_t              sec[MAX_SECT];
    int                 nsec;
    uint32_t            lo, hi;              // the first and last rows with ink
    int                 full;                // the plan outgrew a table
};

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

// ------------------------------------------------------------------ the face

// The check mark that ticks the screen in use is the face's one glyph past 0x7E: text never
// takes it from outside, `tick` draws it.
#define TICK 0x7F
static const uint8_t k_tick[7] = { 0x00, 0x01, 0x01, 0x02, 0x12, 0x0C, 0x04 };
static const char k_tick_text[2] = { TICK, '\0' };

static const uint8_t *glyph(unsigned char c)
{
    if (c == TICK) return k_tick;
    return sw_font[(c >= 0x20 && c < 0x7F ? c : '?') - 0x20];
}

// A line of text at (x, y): the sanitised string kept in the pool. Printable ASCII is kept; a
// byte outside it is one '?' (the continuation bytes of UTF-8 dropped), as the ImageWriter's
// page does. At most `cap` characters. Bold is a dot wider in every pixel.
static void text(tp_t *t, uint32_t x, uint32_t y, const char *s, size_t cap, int bold)
{
    if (!s) s = "";
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
    if (y + GH - 1 > t->hi) t->hi = y + GH - 1;
}

static void tick(tp_t *t, uint32_t x, uint32_t y, int bold)
{
    if (t->ntxt >= MAX_TEXT) { t->full = 1; return; }
    t->txt[t->ntxt++] = (text_t){ (uint16_t)x, (uint16_t)y, 1, (uint8_t)bold, k_tick_text };
}

static void text_row(const tp_t *t, uint32_t y, uint8_t *k)
{
    for (int i = 0; i < t->ntxt; i++) {
        const text_t *it = &t->txt[i];
        if (y < it->y || y >= it->y + GH) continue;
        uint32_t py = (y - it->y) / PX;
        for (uint32_t c = 0; c < it->n; c++) {
            uint8_t g = glyph((unsigned char)it->s[c])[py];
            for (uint32_t col = 0; col < 5; col++)
                if (g >> (4 - col) & 1) span(k, it->x + c * CELL + col * PX, PX + (uint32_t)it->bold);
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

// The six screens, in the order the page shows them.
static const ht_algo_t k_screens[6] = { HT_ATKINSON, HT_FLOYD, HT_BLUENOISE, HT_BAYER, HT_THRESHOLD, HT_APPLE };

// A halftoner as a job makes it for one ink: the screen, the dot gain, the model's tone curve
// for that screen, the ink's place on the tile, and Apple's tile started on its second row
// (ripjob.c's apple_page_start). 0, or -1 for no memory.
static int ht_make(const tp_t *t, ht_t *h, ht_algo_t algo, uint32_t w, iw_ink_t ink)
{
    if (ht_init(h, algo, w)) return -1;
    ht_set_gain(h, t->gain);
    ht_set_tone(h, sw_tone(t->model, algo));
    ink_screen_phase(h, ink);
    if (algo == HT_APPLE) ht_set_phase(h, h->phase_x, h->phase_y + 1, h->invert);
    return 0;
}

// ------------------------------------------------------------------ parts: rules, swatches

static void fill_rule(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)y;
    memset(bits[IW_BLACK] + s->x / 8, 0xFF, s->w / 8);
}

static void fill_swatch(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)y;
    memset(bits[s->ink] + s->x / 8, 0xFF, s->w / 8);
}

// ------------------------------------------------------------------ part: the letterhead

typedef struct {
    ink_screens_t inks;                     // the icon, color cartridge
    ht_t          gray;                     // and without
    uint8_t       rgb[ICON_W * 3], g[ICON_W], ib[IW_INKS][ICON_W / 8];
} head_t;

#define TITLE_X  (ICON_W + 72)
#define TITLE_Y  56
#define SUB_Y    106
#define HEAD_H   (SUB_Y + SW_SUB_H)

static int begin_head(tp_t *t, sect_t *s)
{
    head_t *h = big_calloc(1, sizeof *h);
    if (!h) return -1;
    // The icon is pixel art: the ordered screen keeps its edges, in the inks it is drawn in.
    if (t->colour ? ink_screens_init(&h->inks, HT_BAYER, 0, ICON_W)
                  : ht_init(&h->gray, HT_BAYER, ICON_W)) {
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
    if (y < SW_ICON_H * ICON_S) {
        for (uint32_t x = 0; x < ICON_W; x++)
            memcpy(h->rgb + x * 3, sw_icon_rgb[sw_icon[y / ICON_S][x / ICON_S]], 3);
        if (t->colour) {
            uint8_t *const ib[IW_INKS] = { h->ib[0], h->ib[1], h->ib[2], h->ib[3] };
            ink_screens_row(&h->inks, h->rgb, ib);
            for (int i = 0; i < IW_INKS; i++) put_bits(bits[i], 0, h->ib[i], ICON_W);
        } else {
            uint8_t b[ICON_W / 8];
            ink_gray(h->rgb, ICON_W, h->g);
            ht_row(&h->gray, h->g, b);
            put_bits(bits[IW_BLACK], 0, b, ICON_W);
        }
    }
    if (y >= TITLE_Y && y < TITLE_Y + SW_TITLE_H) blit(bits[IW_BLACK], TITLE_X, sw_title[y - TITLE_Y], SW_TITLE_W);
    if (y >= SUB_Y && y < SUB_Y + SW_SUB_H) blit(bits[IW_BLACK], W - SW_SUB_W, sw_sub[y - SUB_Y], SW_SUB_W);
}

// ------------------------------------------------------------------ part: the still life

#define PHOTO_NB (PHOTO_W / 8)

typedef struct {
    ink_screens_t inks;
    ht_t          gray;
    uint8_t       scene[SCENE_W * 3], rgb[PHOTO_W * 3], g[PHOTO_W], b[PHOTO_NB], ib[IW_INKS][PHOTO_NB];
} photo_t;

static int begin_photo(tp_t *t, sect_t *s)
{
    photo_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    if (t->colour) {
        if (ink_screens_init(&p->inks, t->screen, t->gain, PHOTO_W)) { free(p); return -1; }
        ink_screens_set_tone(&p->inks, sw_tone(t->model, t->screen));
        if (t->screen == HT_APPLE)
            for (int i = 0; i < IW_INKS; i++)
                ht_set_phase(&p->inks.ht[i], p->inks.ht[i].phase_x, p->inks.ht[i].phase_y + 1, p->inks.ht[i].invert);
    } else if (ht_make(t, &p->gray, t->screen, PHOTO_W, IW_BLACK)) {
        free(p);
        return -1;
    }
    s->st = p;
    return 0;
}

static void end_photo(tp_t *t, sect_t *s)
{
    photo_t *p = s->st;
    if (t->colour) ink_screens_free(&p->inks); else ht_free(&p->gray);
    free(p);
}

static void fill_photo(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    photo_t *p = s->st;
    if (y % 2 == 0) {                                   // two rows of dots to a row of the scene
        tp_scene_row_aspect(SCENE_W, SCENE_H, y / 2, p->scene, (float)SCENE_H / SCENE_W);
        for (uint32_t x = 0; x < SCENE_W; x++) {
            memcpy(p->rgb + x * 6, p->scene + x * 3, 3);
            memcpy(p->rgb + x * 6 + 3, p->scene + x * 3, 3);
        }
    }
    if (t->colour) {
        uint8_t *const ib[IW_INKS] = { p->ib[0], p->ib[1], p->ib[2], p->ib[3] };
        ink_screens_row(&p->inks, p->rgb, ib);
        for (int i = 0; i < IW_INKS; i++) put_bits(bits[i], s->x, p->ib[i], PHOTO_W);
    } else {
        ink_gray(p->rgb, PHOTO_W, p->g);
        ht_row(&p->gray, p->g, p->b);
        put_bits(bits[IW_BLACK], s->x, p->b, PHOTO_W);
    }
}

// ------------------------------------------------------------------ part: a ramp of one ink

typedef struct { ht_t h; uint8_t g[SIDE_W], b[SIDE_W / 8]; } ramp_t;

static int begin_ramp(tp_t *t, sect_t *s)
{
    ramp_t *r = big_calloc(1, sizeof *r);
    if (!r) return -1;
    if (ht_make(t, &r->h, t->screen, SIDE_W, (iw_ink_t)s->ink)) { free(r); return -1; }
    for (uint32_t x = 0; x < SIDE_W; x++) r->g[x] = (uint8_t)(255.0 - 255.0 * x / (SIDE_W - 1));
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
    put_bits(bits[s->ink], s->x, r->b, SIDE_W);
}

// ------------------------------------------------------------------ part: eleven gray steps

typedef struct { ht_t h[11]; uint8_t g[STEP_W], b[STEP_W / 8]; } steps_t;

static int begin_steps(tp_t *t, sect_t *s)
{
    steps_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    for (int j = 0; j < 11; j++)
        if (ht_make(t, &p->h[j], t->screen, STEP_W, IW_BLACK)) {
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
    (void)t; (void)y;
    steps_t *p = s->st;
    for (int j = 0; j < 11; j++) {
        memset(p->g, 255 - 255 * j / 10, STEP_W);
        ht_row(&p->h[j], p->g, p->b);
        put_bits(bits[IW_BLACK], s->x + (uint32_t)j * STEP_DX, p->b, STEP_W);
    }
}

// ------------------------------------------------------------------ part: the six screens

typedef struct { ht_t h[6]; uint8_t rgb[PANEL_W * 3], g[PANEL_W], b[PANEL_W / 8]; } panels_t;

static int begin_panels(tp_t *t, sect_t *s)
{
    panels_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    for (int i = 0; i < 6; i++)
        if (ht_make(t, &p->h[i], k_screens[i], PANEL_W, IW_BLACK)) {
            while (i--) ht_free(&p->h[i]);
            free(p);
            return -1;
        }
    s->st = p;
    return 0;
}

static void end_panels(tp_t *t, sect_t *s)
{
    (void)t;
    panels_t *p = s->st;
    for (int i = 0; i < 6; i++) ht_free(&p->h[i]);
    free(p);
}

// The still life through each screen, in black alone, and under it a ramp.
static void fill_panels(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t;
    panels_t *p = s->st;
    if (s->idx == 0) {
        tp_scene_row_aspect(PANEL_W, PANEL_H, y, p->rgb, (float)PANEL_H / PANEL_W);
        ink_gray(p->rgb, PANEL_W, p->g);
    } else {
        for (uint32_t x = 0; x < PANEL_W; x++) p->g[x] = (uint8_t)(255.0 - 255.0 * x / (PANEL_W - 1));
    }
    for (int i = 0; i < 6; i++) {
        ht_row(&p->h[i], p->g, p->b);
        put_bits(bits[IW_BLACK], (uint32_t)i * PANEL_DX, p->b, PANEL_W);
    }
}

// ------------------------------------------------------------------ parts: ruler, squares

#define RULER_H    64
#define RULER_EDGE 4
#define TICK_TENTH 16
#define TICK_HALF  32
#define TICK_INCH  56

// 8 in with a tick every 36 dots, 0.1 in: the half inch longer, the inch longer still. The edge
// is four rows thick, so that one dead nozzle thins it and cannot break it.
static void fill_ruler(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)s;
    uint8_t *k = bits[IW_BLACK];
    if (y < RULER_EDGE) { memset(k, 0xFF, W / 8); return; }
    uint32_t d = y - RULER_EDGE;
    for (uint32_t i = 0; i <= 80; i++) {
        uint32_t len = i % 10 == 0 ? TICK_INCH : i % 5 == 0 ? TICK_HALF : TICK_TENTH;
        if (d < len) span(k, i < 80 ? i * 36 : W - 2, 2);       // (the last one inside the 8 in)
    }
}

#define SQ 360

// Four squares an inch each way, 720 dots apart: a quarter-inch grid, solid black, vertical
// lines every 8 dots and the finest checker. Each fills its 360 x 360 dots corner to corner.
static void fill_squares(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)t; (void)s;
    uint8_t *k = bits[IW_BLACK];
    // 1: the grid
    if (y % 90 < 2 || y >= SQ - 2) {
        span(k, 0, SQ);
    } else {
        for (uint32_t x = 0; x < SQ; x += 90) span(k, x, 2);
        span(k, SQ - 2, 2);
    }
    // 2: solid
    memset(k + 720 / 8, 0xFF, SQ / 8);
    // 3: lines, framed
    if (y < 2 || y >= SQ - 2) {
        span(k, 1440, SQ);
    } else {
        for (uint32_t x = 0; x < SQ; x += 8) span(k, 1440 + x, 2);
        span(k, 1440 + SQ - 2, 2);
    }
    // 4: the checker
    for (uint32_t x = 0; x < SQ; x++)
        if ((x + y) % 2 == 0) setb(k, 2160 + x);
}

// ------------------------------------------------------------------ the plan

static sw_tp_box_t bx(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return (sw_tp_box_t){ (uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h };
}

static void push(tp_t *t, sect_t s)
{
    if (t->nsec >= MAX_SECT) { t->full = 1; return; }
    t->sec[t->nsec++] = s;
    if (s.y0 < t->lo) t->lo = s.y0;
    if (s.y0 + s.rows - 1 > t->hi) t->hi = s.y0 + s.rows - 1;
}

static void rule(tp_t *t, uint32_t y)
{
    push(t, (sect_t){ .y0 = y, .rows = 3, .x = 0, .w = W, .fill = fill_rule });
}

// A heading: bold, with a rule under it. The part under it starts 64 dots on.
static uint32_t heading(tp_t *t, uint32_t y, const char *s)
{
    text(t, 0, y, s, 60, 1);
    rule(t, y + 40);
    return y + 64;
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

static void plan(tp_t *t)
{
    const sw_spec_t *sp = sw_spec(t->model);
    sw_tp_layout_t *L = &t->lay;
    uint32_t y = TOP;
    t->lo = UINT32_MAX;
    t->hi = 0;

    // The letterhead, and a rule.
    L->letterhead = bx(0, y, W, HEAD_H);
    push(t, (sect_t){ .y0 = y, .rows = HEAD_H, .begin = begin_head, .fill = fill_head, .end = end_head });
    y += HEAD_H + 16;
    rule(t, y);
    y += 3 + 56;

    // The board, in two columns: labels 11 characters, then the values.
    {
        const sw_tp_info_t *i = t->info;
        char printer[96], page[80], margins[80];
        snprintf(printer, sizeof printer, "%s%s", sp->name,
                 !sp->colour ? "" : t->colour ? ", color cartridge" : ", black cartridge");
        uint32_t pw = sw_printable_w(t->model, SW_TP_SHEET_W);
        snprintf(page, sizeof page, "Letter, %u x %u dots at 360 dpi", (unsigned)pw,
                 (unsigned)(SW_TP_SHEET_H - sp->margin_t - sp->margin_b));
        snprintf(margins, sizeof margins, "%u left, %u top, %u bottom dots", (unsigned)sp->margin_l,
                 (unsigned)sp->margin_t, (unsigned)sp->margin_b);
        char url[96];
        snprintf(url, sizeof url, "http://%s", i->host ? i->host : "");
        const uint32_t lx = 0, lv = 11 * CELL, rx = 60 * CELL, rv = rx + 11 * CELL;
        L->details = bx(0, y, W, 3 * LINE + GH);
        text(t, lx, y, "Printer", 11, 0);             text(t, lv, y, printer, 48, 0);
        text(t, rx, y, "AirWriter", 11, 0);           text(t, rv, y, i->board, 48, 0);
        y += LINE;
        text(t, lx, y, "Name", 11, 0);                text(t, lv, y, i->printer, 48, 0);
        text(t, rx, y, "Halftone", 11, 0);
        if (t->gain > 0) textf(t, rv, y, 48, 0, "%s, dot gain %d%%", screen_name(t->screen), t->gain);
        else             text(t, rv, y, screen_name(t->screen), 48, 0);
        y += LINE;
        text(t, lx, y, "Settings", 11, 0);            text(t, lv, y, url, 48, 0);
        text(t, rx, y, "Page", 11, 0);                text(t, rv, y, page, 48, 0);
        y += LINE;
        text(t, rx, y, "Margins", 11, 0);             text(t, rv, y, margins, 48, 0);
        y += LINE + GAP - 20;
    }

    // Ink: four solid swatches, the still life and a ramp of each ink; or, without a color
    // cartridge, the still life in gray beside eleven steps and a ramp of black.
    if (t->colour) {
        y = heading(t, y, "Color");
        for (int i = 0; i < 4; i++) {
            uint32_t x = (uint32_t)i * 760;
            push(t, (sect_t){ .y0 = y, .rows = 150, .x = x, .w = 600, .ink = k_ink_of[i], .fill = fill_swatch });
            L->swatch[i] = bx(x, y, 600, 150);
            text(t, x, y + 150 + 16, k_ink_name[i], 20, 0);
        }
        y += 150 + 16 + GH + 64;
        push(t, (sect_t){ .y0 = y, .rows = PHOTO_H, .x = 0, .begin = begin_photo, .fill = fill_photo, .end = end_photo });
        L->photo = bx(0, y, PHOTO_W, PHOTO_H);
        for (int i = 0; i < 4; i++) {
            uint32_t ry = y + (uint32_t)i * 170;
            text(t, SIDE_X, ry, k_ink_name[i], 20, 0);
            push(t, (sect_t){ .y0 = ry + 44, .rows = 80, .x = SIDE_X, .ink = k_ink_of[i],
                              .begin = begin_ramp, .fill = fill_ramp, .end = end_ramp });
            L->ink_ramp[i] = bx(SIDE_X, ry + 44, SIDE_W, 80);
        }
        y += PHOTO_H + GAP;
    } else {
        y = heading(t, y, "Photo");
        if (sp->colour) {
            text(t, 0, y, "The black cartridge is in. With a color cartridge in, this part prints in color.", 80, 0);
            y += LINE + 12;
        }
        push(t, (sect_t){ .y0 = y, .rows = PHOTO_H, .x = 0, .begin = begin_photo, .fill = fill_photo, .end = end_photo });
        L->photo = bx(0, y, PHOTO_W, PHOTO_H);
        push(t, (sect_t){ .y0 = y, .rows = 340, .x = SIDE_X, .begin = begin_steps, .fill = fill_steps, .end = end_steps });
        for (int j = 0; j < 11; j++) {
            uint32_t x = SIDE_X + (uint32_t)j * STEP_DX;
            L->gray_step[j] = bx(x, y, STEP_W, 340);
            textf(t, x, y + 356, 5, 0, "%d%%", j * 10);
        }
        text(t, SIDE_X, y + 470, "Black", 20, 0);
        push(t, (sect_t){ .y0 = y + 514, .rows = 80, .x = SIDE_X, .ink = IW_BLACK,
                          .begin = begin_ramp, .fill = fill_ramp, .end = end_ramp });
        L->ink_ramp[3] = bx(SIDE_X, y + 514, SIDE_W, 80);
        y += PHOTO_H + GAP;
    }

    // The six screens side by side, the one in use ticked.
    y = heading(t, y, "Halftone");
    L->screen_names = bx(0, y, W, GH);
    for (int i = 0; i < 6; i++) {
        uint32_t x = (uint32_t)i * PANEL_DX;
        int on = k_screens[i] == t->screen;
        const char *name = screen_name(k_screens[i]);
        text(t, x, y, name, 18, on);
        if (on) tick(t, x + ((uint32_t)strlen(name) + 1) * CELL, y, 1);
        L->panel_screen[i] = k_screens[i];
        L->panel[i] = bx(x, y + 44, PANEL_W, PANEL_H);
        L->panel_ramp[i] = bx(x, y + 44 + PANEL_H + 12, PANEL_W, PRAMP_H);
    }
    push(t, (sect_t){ .y0 = y + 44, .rows = PANEL_H, .idx = 0, .begin = begin_panels, .fill = fill_panels, .end = end_panels });
    push(t, (sect_t){ .y0 = y + 44 + PANEL_H + 12, .rows = PRAMP_H, .idx = 1, .begin = begin_panels, .fill = fill_panels, .end = end_panels });
    y += 44 + PANEL_H + 12 + PRAMP_H + 32;
    text(t, 0, y, "The ticked screen prints your pages. Change it in the printer's settings.", 100, 0);
    y += GH + GAP;

    // Alignment: a ruler 8 in long, numbered, and four squares an inch each way with captions.
    y = heading(t, y, "Alignment");
    push(t, (sect_t){ .y0 = y, .rows = RULER_H, .fill = fill_ruler });
    L->ruler = bx(0, y, W, RULER_H);
    for (uint32_t i = 0; i <= 8; i++) {
        char d[2] = { (char)('0' + i), '\0' };
        text(t, i < 8 ? i * 360 : W - CELL, y + RULER_H + 12, d, 1, 0);
    }
    y += RULER_H + 12 + GH + 72;
    push(t, (sect_t){ .y0 = y, .rows = SQ, .fill = fill_squares });
    for (int i = 0; i < 4; i++) L->square[i] = bx((uint32_t)i * 720, y, SQ, SQ);
    y += SQ + 28;
    static const char *const caps[2][4] = {
        { "One inch each way", "Solid black", "Straight lines", "Even gray" },
        { "Measure it with a ruler", "White line: a nozzle is out", "The head is aligned", "The paper feeds evenly" },
    };
    for (int r = 0; r < 2; r++, y += LINE)
        for (int i = 0; i < 4; i++) text(t, (uint32_t)i * 720, y, caps[r][i], 30, 0);
    y += GAP - LINE + 20;

    // The footer.
    rule(t, y);
    y += 3 + 24;
    L->footer = bx(0, y, W, GH);
    text(t, 0, y, "AirWriter by Wells Workshop    wells.ee    github.com/wr/airwriter", 100, 0);

    L->first_ink = (uint16_t)t->lo;
    L->last_ink = (uint16_t)t->hi;
    L->colour = t->colour;
}

static void setup(tp_t *t, const sw_tp_info_t *i)
{
    t->info = i;
    t->model = i->model;
    t->colour = i->colour_cart && sw_spec(i->model)->colour;
    t->screen = i->screen;
    t->gain = i->gain;
    plan(t);
}

void sw_testpage_layout(const sw_tp_info_t *i, sw_tp_layout_t *out)
{
    memset(out, 0, sizeof *out);
    if (!i || !sw_spec(i->model)) return;
    tp_t *t = big_calloc(1, sizeof *t);
    if (!t) return;
    setup(t, i);
    *out = t->lay;
    free(t);
}

// ------------------------------------------------------------------ the page

int sw_testpage(const sw_tp_info_t *i, const rip_stage_t *st)
{
    const sw_spec_t *sp = i ? sw_spec(i->model) : NULL;
    if (!sp) return SW_TP_NOMEM;
    int colour = i->colour_cart && sp->colour;
    if (!st || !st->page_begin || !st->row || !st->page_end || (colour && !st->ink_row))
        return SW_TP_STAGE;                  // a stage that cannot take this page's rows
    uint32_t pw = sw_printable_w(i->model, SW_TP_SHEET_W), rb = (pw + 7) / 8;
    if (pw < W) return SW_TP_NOMEM;          // (no model has less than 8 in across)

    tp_t *t = big_calloc(1, sizeof *t);
    uint8_t *rows = big_calloc(IW_INKS, rb);
    if (!t || !rows) {
        free(t);
        free(rows);
        return SW_TP_NOMEM;
    }
    setup(t, i);
    uint8_t *planes[IW_INKS];
    for (int k = 0; k < IW_INKS; k++) planes[k] = rows + (size_t)k * rb;
    int rc = t->full ? SW_TP_NOMEM : SW_TP_DONE;

    if (rc == SW_TP_DONE && st->page_begin(st->ctx, 0, pw, SW_TP_SHEET_H)) rc = SW_TP_STAGE;
    for (uint32_t sy = 0; rc == SW_TP_DONE && sy < SW_TP_SHEET_H; sy++) {
        if ((sy & 31) == 0 && i->stop && i->stop(i->stop_ctx)) { rc = SW_TP_STOPPED; break; }
        memset(rows, 0, (size_t)IW_INKS * rb);
        // The layout starts under the engine's top margin; the margins are blank rows.
        if (sy >= sp->margin_t && sy - sp->margin_t <= t->hi) {
            uint32_t y = sy - sp->margin_t;
            for (int n = 0; n < t->nsec && rc == SW_TP_DONE; n++) {
                sect_t *s = &t->sec[n];
                if (y < s->y0 || y >= s->y0 + s->rows) continue;
                if (s->state == PART_NEW) {
                    if (s->begin && s->begin(t, s)) { rc = SW_TP_NOMEM; break; }
                    s->state = PART_HELD;
                }
                s->fill(t, s, y - s->y0, planes);
                if (y == s->y0 + s->rows - 1) {
                    if (s->end) s->end(t, s);
                    s->state = PART_DONE;
                }
            }
            text_row(t, y, planes[IW_BLACK]);
        }
        if (rc != SW_TP_DONE) break;
        if (colour) st->ink_row(st->ctx, (const uint8_t *const *)planes);
        else        st->row(st->ctx, planes[IW_BLACK]);
    }
    if (rc == SW_TP_DONE && st->page_end(st->ctx)) rc = SW_TP_STAGE;

    // Whatever a part still holds (a page stopped or short of memory partway).
    for (int n = 0; n < t->nsec; n++)
        if (t->sec[n].state == PART_HELD && t->sec[n].end) t->sec[n].end(t, &t->sec[n]);
    free(rows);
    free(t);
    return rc;
}
