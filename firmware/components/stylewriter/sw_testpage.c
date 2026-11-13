// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter's test page (sw_testpage.h). The page is a list of parts, each a run of rows
// with a function that fills one row of it, and a list of lines of text the face draws over
// them: the same idea as the ImageWriter's bands (testpage.c's fill_fn), at 360 dpi and square
// dots, one row at a time rather than in strips. Parts that screen a picture or a ramp make
// their halftoners when their first row comes and free them after their last, so only the
// parts on a row are held. All sizes are in dots at 360 dpi from the printable area's
// top-left, scaled to the page's dpi where they are used (S, S8: the sizes themselves on a
// StyleWriter's page); `plan` lays the page out once and the same plan serves the page and the
// layout the tests read.
//
// Everything that grows is through bigmem.h; a row of text, a rule, the ruler and the squares
// are drawn into the row's planes a dot at a time and cost nothing but time.
#include "sw_testpage.h"
#include "sw_font.h"
#include "sw_art.h"
#include "sw_scene.h"
#include "sw_unpack.h"
#include "ink.h"
#include "bigmem.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define W        SW_TP_W            // the layout is 8 in across
#define PX       4                  // dots to a pixel of the face (6 a cell, 7 a glyph)
#define LINE     60                 // from one line of text to the next
#define TOP      48                 // clear paper over the letterhead
#define GAP      110                // between one part of the page and the next

// Where the pictures are, all multiples of 8 across so a halftoned row is copied in by the byte.
#define PHOTO_W  1264
#define PHOTO_H  700
#define SCENE_W  (PHOTO_W / 2)      // the still life is stored at 180 dpi and the screen does the rest
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
#define ICON_MAX (SW_ICON_W * ICON_S)  // the widest letterhead icon, in dots

// The pictures are traced once on the host (tools/rip/mkscene.c, W-1133), at these sizes.
_Static_assert(SW_SCENE_W == SCENE_W && SW_SCENE_H == SCENE_H, "sw_scene.h: make -C tools/rip scene");
_Static_assert(SW_PANEL_W == PANEL_W && SW_PANEL_H == PANEL_H, "sw_scene.h: make -C tools/rip scene");

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
    const sw_tp_geom_t *geom;               // NULL: a StyleWriter's page
    const sw_tp_art_t  *art;
    sw_model_t          model;
    int                 colour;
    ht_algo_t           screen;             // the record's, HT_APPLE for the family's Apple
    int                 gain;
    // The page's dpi and the sizes it scales once (the 360 dpi ones on a StyleWriter's).
    uint32_t            dpi, w, px, cell, gh;
    uint32_t            icon_s, icon_w, title_x, title_y, sub_y, head_h;
    uint32_t            photo_w, photo_h, side_x, side_w, step_w, step_dx;
    uint32_t            panel_w, panel_h, panel_dx;
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

// ------------------------------------------------------------------ sizes

// A size in dots at 360 dpi, at the page's dpi, rounded; and the same down to a whole byte, for
// a picture's columns. Both are the size itself at 360 dpi.
static uint32_t scale(uint32_t dpi, uint32_t v) { return (v * dpi + 180) / 360; }
static uint32_t S(const tp_t *t, uint32_t v) { return scale(t->dpi, v); }
static uint32_t S8(const tp_t *t, uint32_t v) { return S(t, v) & ~7u; }

// The StyleWriter's letterhead (sw_art.h, at 360 dpi).
static const sw_tp_art_t k_sw_art = {
    .title_w = SW_TITLE_W, .title_h = SW_TITLE_H, .sub_w = SW_SUB_W, .sub_h = SW_SUB_H,
    .icon_w = SW_ICON_W, .icon_h = SW_ICON_H,
    .title = &sw_title[0][0], .sub = &sw_sub[0][0], .icon = &sw_icon[0][0], .rgb = sw_icon_rgb,
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
    if (y + t->gh - 1 > t->hi) t->hi = y + t->gh - 1;
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
        if (y < it->y || y >= it->y + t->gh) continue;
        uint32_t py = (y - it->y) / t->px;
        for (uint32_t c = 0; c < it->n; c++) {
            uint8_t g = glyph((unsigned char)it->s[c])[py];
            for (uint32_t col = 0; col < 5; col++)
                if (g >> (4 - col) & 1)
                    span(k, it->x + c * t->cell + col * t->px, t->px + (uint32_t)it->bold);
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
// (ripjob.c's apple_page_start). Another family's is its gray rows' (ripjob.c, no tone): the
// screen, its own Apple screen for HT_APPLE, and the gain. 0, or -1 for no memory.
static int ht_make(const tp_t *t, ht_t *h, ht_algo_t algo, uint32_t w, iw_ink_t ink)
{
    if (t->geom) {
        if (ht_init(h, algo == HT_APPLE ? t->geom->apple : algo, w)) return -1;
        ht_set_gain(h, t->gain);
        return 0;
    }
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
    ink_screens_t inks;                     // the icon, color cartridge
    ht_t          gray;                     // and without
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
    const sw_tp_art_t *a = t->art;
    if (y < a->icon_h * t->icon_s) {
        const uint8_t *row = a->icon + (size_t)(y / t->icon_s) * a->icon_w;
        for (uint32_t x = 0; x < t->icon_w; x++) memcpy(h->rgb + x * 3, a->rgb[row[x / t->icon_s]], 3);
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
    if (y >= t->title_y && y < t->title_y + a->title_h)
        blit(bits[IW_BLACK], t->title_x, a->title + (size_t)(y - t->title_y) * ((a->title_w + 7u) / 8), a->title_w);
    if (y >= t->sub_y && y < t->sub_y + a->sub_h)
        blit(bits[IW_BLACK], t->w - a->sub_w, a->sub + (size_t)(y - t->sub_y) * ((a->sub_w + 7u) / 8), a->sub_w);
}

// ------------------------------------------------------------------ part: the still life

#define PHOTO_NB (PHOTO_W / 8)

typedef struct {
    ink_screens_t inks;
    ht_t          gray;
    sw_unpack_t  *z;                    // the stored still life, a row of the scene at a time
    uint32_t      zrows;                // the scene's rows read
    uint8_t       scene[SCENE_W * 3], rgb[PHOTO_W * 3], g[PHOTO_W], b[PHOTO_NB], ib[IW_INKS][PHOTO_NB];
} photo_t;

static int begin_photo(tp_t *t, sect_t *s)
{
    photo_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    p->z = sw_unpack_new(sw_scene_photo_z, sizeof sw_scene_photo_z);
    if (!p->z) { free(p); return -1; }
    if (t->colour) {
        if (ink_screens_init(&p->inks, t->screen, t->gain, s->w)) { sw_unpack_free(p->z); free(p); return -1; }
        ink_screens_set_tone(&p->inks, sw_tone(t->model, t->screen));
        if (t->screen == HT_APPLE)
            for (int i = 0; i < IW_INKS; i++)
                ht_set_phase(&p->inks.ht[i], p->inks.ht[i].phase_x, p->inks.ht[i].phase_y + 1, p->inks.ht[i].invert);
    } else if (ht_make(t, &p->gray, t->screen, s->w, IW_BLACK)) {
        sw_unpack_free(p->z);
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
    sw_unpack_free(p->z);
    free(p);
}

static void fill_photo(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    photo_t *p = s->st;
    // The scene's rows and columns to the picture's (two dots to a pixel of it at 360 dpi).
    const uint32_t sy = y * SCENE_H / s->rows;
    if (p->zrows <= sy) {
        // The stream is ours and held to tp_scene.c by the tests: a row it cannot give is paper.
        while (p->zrows <= sy) {
            if (sw_unpack_read(p->z, p->scene, sizeof p->scene)) memset(p->scene, 255, sizeof p->scene);
            p->zrows++;
        }
        for (uint32_t x = 0; x < s->w; x++) memcpy(p->rgb + x * 3, p->scene + x * SCENE_W / s->w * 3, 3);
    }
    if (t->colour) {
        uint8_t *const ib[IW_INKS] = { p->ib[0], p->ib[1], p->ib[2], p->ib[3] };
        ink_screens_row(&p->inks, p->rgb, ib);
        for (int i = 0; i < IW_INKS; i++) put_bits(bits[i], s->x, p->ib[i], s->w);
    } else {
        ink_gray(p->rgb, s->w, p->g);
        ht_row(&p->gray, p->g, p->b);
        put_bits(bits[IW_BLACK], s->x, p->b, s->w);
    }
}

// ------------------------------------------------------------------ part: a ramp of one ink

typedef struct { ht_t h; uint8_t g[SIDE_W], b[SIDE_W / 8]; } ramp_t;

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

// ------------------------------------------------------------------ part: the six screens

// idx 0 is the panels themselves, which read the stored scene in gray; idx 1 the ramps under them.
typedef struct {
    ht_t         h[6];
    sw_unpack_t *z;
    uint32_t     zrows;                 // the scene's rows read
    uint8_t      src[SW_PANEL_W], g[PANEL_W], b[PANEL_W / 8];
} panels_t;

static int begin_panels(tp_t *t, sect_t *s)
{
    panels_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    if (s->idx == 0 && !(p->z = sw_unpack_new(sw_scene_panel_z, sizeof sw_scene_panel_z))) {
        free(p);
        return -1;
    }
    for (int i = 0; i < 6; i++)
        if (ht_make(t, &p->h[i], k_screens[i], t->panel_w, IW_BLACK)) {
            while (i--) ht_free(&p->h[i]);
            sw_unpack_free(p->z);
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
    sw_unpack_free(p->z);
    free(p);
}

// The still life through each screen, in black alone, and under it a ramp. The stored scene is
// the panel's size at 360 dpi; at another, its rows and columns are picked to the panel's.
static void fill_panels(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    panels_t *p = s->st;
    const uint32_t pw = t->panel_w;
    if (s->idx == 0) {
        const uint32_t sy = y * SW_PANEL_H / s->rows;
        while (p->zrows <= sy) {
            if (sw_unpack_read(p->z, p->src, sizeof p->src)) memset(p->src, 255, sizeof p->src);
            p->zrows++;
        }
        for (uint32_t x = 0; x < pw; x++) p->g[x] = p->src[x * SW_PANEL_W / pw];
    } else {
        for (uint32_t x = 0; x < pw; x++) p->g[x] = (uint8_t)(255.0 - 255.0 * x / (pw - 1));
    }
    for (int i = 0; i < 6; i++) {
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
// is four rows thick, so that one dead nozzle thins it and cannot break it.
static void fill_ruler(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)s;
    uint8_t *k = bits[IW_BLACK];
    if (y < S(t, RULER_EDGE)) { memset(k, 0xFF, t->w / 8); return; }
    uint32_t d = y - S(t, RULER_EDGE), tw = S(t, 2);
    for (uint32_t i = 0; i <= 80; i++) {
        uint32_t len = S(t, i % 10 == 0 ? TICK_INCH : i % 5 == 0 ? TICK_HALF : TICK_TENTH);
        if (d < len) span(k, i < 80 ? S(t, i * 36) : t->w - tw, tw);   // (the last one inside the 8 in)
    }
}

#define SQ 360

// Four squares an inch each way, 720 dots apart: a quarter-inch grid, solid black, vertical
// lines every 8 dots and the finest checker. Each fills its 360 x 360 dots corner to corner.
static void fill_squares(tp_t *t, sect_t *s, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)s;
    uint8_t *k = bits[IW_BLACK];
    const uint32_t sq = S(t, SQ), th = S(t, 2), quarter = S(t, 90), pitch = S(t, 8);
    // 1: the grid
    if (y % quarter < th || y >= sq - th) {
        span(k, 0, sq);
    } else {
        for (uint32_t x = 0; x < sq; x += quarter) span(k, x, th);
        span(k, sq - th, th);
    }
    // 2: solid
    span(k, S(t, 720), sq);
    // 3: lines, framed
    const uint32_t lx = S(t, 1440);
    if (y < th || y >= sq - th) {
        span(k, lx, sq);
    } else {
        for (uint32_t x = 0; x < sq; x += pitch) span(k, lx + x, th);
        span(k, lx + sq - th, th);
    }
    // 4: the checker
    const uint32_t cx = S(t, 2160);
    for (uint32_t x = 0; x < sq; x++)
        if ((x + y) % 2 == 0) setb(k, cx + x);
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
    push(t, (sect_t){ .y0 = y, .rows = S(t, 3), .x = 0, .w = t->w, .fill = fill_rule });
}

// A heading: bold, with a rule under it. The part under it starts 64 dots on.
static uint32_t heading(tp_t *t, uint32_t y, const char *s)
{
    text(t, 0, y, s, 60, 1);
    rule(t, y + S(t, 40));
    return y + S(t, 64);
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

// The colour row: the four inks, then the three colours two of them make printed over each
// other, as the ImageWriter's colour ribbon page shows its overprints (W-1134).
#define INK(k) (1 << (k))
static const char *const k_swatch_name[SW_TP_SWATCHES] = { "Cyan", "Magenta", "Yellow", "Black",
                                                           "Red", "Green", "Blue" };
static const int k_swatch_inks[SW_TP_SWATCHES] = {
    INK(IW_CYAN), INK(IW_MAGENTA), INK(IW_YELLOW), INK(IW_BLACK),
    INK(IW_MAGENTA) | INK(IW_YELLOW), INK(IW_CYAN) | INK(IW_YELLOW), INK(IW_CYAN) | INK(IW_MAGENTA),
};
#define SWATCH_W  336               // nearly an inch, whole bytes, the gaps near the panels' below
#define SWATCH_DX 424
#define SWATCH_H  150
_Static_assert(SWATCH_W % 8 == 0 && SWATCH_DX % 8 == 0, "a swatch is filled by the byte");
_Static_assert((SW_TP_SWATCHES - 1) * SWATCH_DX + SWATCH_W == W, "the row spans the page");

static void plan(tp_t *t)
{
    const sw_spec_t *sp = t->geom ? NULL : sw_spec(t->model);
    const sw_tp_geom_t *g = t->geom;
    sw_tp_layout_t *L = &t->lay;
    const uint32_t W_ = t->w, CELL_ = t->cell, GH_ = t->gh, LINE_ = S(t, LINE), GAP_ = S(t, GAP);
    uint32_t y = S(t, TOP);
    t->lo = UINT32_MAX;
    t->hi = 0;

    // The letterhead, and a rule.
    L->letterhead = bx(0, y, W_, t->head_h);
    push(t, (sect_t){ .y0 = y, .rows = t->head_h, .begin = begin_head, .fill = fill_head, .end = end_head });
    y += t->head_h + S(t, 16);
    rule(t, y);
    y += S(t, 3) + S(t, 56);

    // The board, in two columns: labels 11 characters, then the values.
    {
        const sw_tp_info_t *i = t->info;
        char printer[96], page[80], margins[80];
        uint32_t pw, rows, ml, mt, mb;
        if (g) {
            snprintf(printer, sizeof printer, "%s", g->model_name ? g->model_name : "");
            pw = g->sheet_w;
            rows = (uint32_t)g->sheet_h - g->margin_t - g->margin_b;
            ml = g->margin_l, mt = g->margin_t, mb = g->margin_b;
        } else {
            snprintf(printer, sizeof printer, "%s%s", sp->name,
                     !sp->colour ? "" : t->colour ? ", color cartridge" : ", black cartridge");
            pw = sw_printable_w(t->model, SW_TP_SHEET_W);
            rows = SW_TP_SHEET_H - sp->margin_t - sp->margin_b;
            ml = sp->margin_l, mt = sp->margin_t, mb = sp->margin_b;
        }
        snprintf(page, sizeof page, "Letter, %u x %u dots at %u dpi", (unsigned)pw, (unsigned)rows,
                 (unsigned)t->dpi);
        snprintf(margins, sizeof margins, "%u left, %u top, %u bottom dots", (unsigned)ml,
                 (unsigned)mt, (unsigned)mb);
        char url[96];
        snprintf(url, sizeof url, "http://%s", i->host ? i->host : "");
        const uint32_t lx = 0, lv = 11 * CELL_, rx = 60 * CELL_, rv = rx + 11 * CELL_;
        L->details = bx(0, y, W_, 3 * LINE_ + GH_);
        text(t, lx, y, "Printer", 11, 0);             text(t, lv, y, printer, 48, 0);
        text(t, rx, y, "AirWriter", 11, 0);           text(t, rv, y, i->board, 48, 0);
        y += LINE_;
        text(t, lx, y, "Name", 11, 0);                text(t, lv, y, i->printer, 48, 0);
        text(t, rx, y, "Halftone", 11, 0);
        if (t->gain > 0) textf(t, rv, y, 48, 0, "%s, dot gain %d%%", screen_name(t->screen), t->gain);
        else             text(t, rv, y, screen_name(t->screen), 48, 0);
        y += LINE_;
        text(t, lx, y, "Settings", 11, 0);            text(t, lv, y, url, 48, 0);
        text(t, rx, y, "Page", 11, 0);                text(t, rv, y, page, 48, 0);
        y += LINE_;
        text(t, rx, y, "Margins", 11, 0);             text(t, rv, y, margins, 48, 0);
        y += S(t, LINE + GAP - 20);
    }

    // Ink: seven solid swatches (the four inks and the three they make in pairs), the still
    // life and a ramp of each ink; or, without a color cartridge, the still life in gray beside
    // eleven steps and a ramp of black.
    if (t->colour) {
        y = heading(t, y, "Color");
        const uint32_t sw = S8(t, SWATCH_W), sh = S(t, SWATCH_H);
        for (int i = 0; i < SW_TP_SWATCHES; i++) {
            uint32_t x = S8(t, (uint32_t)i * SWATCH_DX);
            push(t, (sect_t){ .y0 = y, .rows = sh, .x = x, .w = sw, .idx = k_swatch_inks[i],
                              .fill = fill_swatch });
            L->swatch[i] = bx(x, y, sw, sh);
            text(t, x, y + sh + S(t, 16), k_swatch_name[i], 20, 0);
        }
        y += sh + S(t, 16) + GH_ + S(t, 64);
        push(t, (sect_t){ .y0 = y, .rows = t->photo_h, .x = 0, .w = t->photo_w,
                          .begin = begin_photo, .fill = fill_photo, .end = end_photo });
        L->photo = bx(0, y, t->photo_w, t->photo_h);
        for (int i = 0; i < 4; i++) {
            uint32_t ry = y + S(t, (uint32_t)i * 170);
            text(t, t->side_x, ry, k_ink_name[i], 20, 0);
            push(t, (sect_t){ .y0 = ry + S(t, 44), .rows = S(t, 80), .x = t->side_x, .w = t->side_w,
                              .ink = k_ink_of[i], .begin = begin_ramp, .fill = fill_ramp, .end = end_ramp });
            L->ink_ramp[i] = bx(t->side_x, ry + S(t, 44), t->side_w, S(t, 80));
        }
        y += t->photo_h + GAP_;
    } else {
        y = heading(t, y, "Photo");
        if (sp && sp->colour) {
            text(t, 0, y, "The black cartridge is in. With a color cartridge in, this part prints in color.", 80, 0);
            y += LINE_ + S(t, 12);
        }
        push(t, (sect_t){ .y0 = y, .rows = t->photo_h, .x = 0, .w = t->photo_w,
                          .begin = begin_photo, .fill = fill_photo, .end = end_photo });
        L->photo = bx(0, y, t->photo_w, t->photo_h);
        const uint32_t steps_h = S(t, 340);
        push(t, (sect_t){ .y0 = y, .rows = steps_h, .x = t->side_x, .w = t->step_w,
                          .begin = begin_steps, .fill = fill_steps, .end = end_steps });
        for (int j = 0; j < 11; j++) {
            uint32_t x = t->side_x + (uint32_t)j * t->step_dx;
            L->gray_step[j] = bx(x, y, t->step_w, steps_h);
            textf(t, x, y + S(t, 356), 5, 0, "%d%%", j * 10);
        }
        text(t, t->side_x, y + S(t, 470), "Black", 20, 0);
        push(t, (sect_t){ .y0 = y + S(t, 514), .rows = S(t, 80), .x = t->side_x, .w = t->side_w,
                          .ink = IW_BLACK, .begin = begin_ramp, .fill = fill_ramp, .end = end_ramp });
        L->ink_ramp[3] = bx(t->side_x, y + S(t, 514), t->side_w, S(t, 80));
        y += t->photo_h + GAP_;
    }

    // The six screens side by side, the one in use ticked.
    y = heading(t, y, "Halftone");
    L->screen_names = bx(0, y, W_, GH_);
    L->ticked = -1;
    const uint32_t py = y + S(t, 44), ry = py + t->panel_h + S(t, 12), rh = S(t, PRAMP_H);
    for (int i = 0; i < 6; i++) {
        uint32_t x = (uint32_t)i * t->panel_dx;
        int on = k_screens[i] == t->screen;
        const char *name = screen_name(k_screens[i]);
        text(t, x, y, name, 18, on);
        if (on) {
            tick(t, x + ((uint32_t)strlen(name) + 1) * CELL_, y, 1);
            L->ticked = i;
        }
        L->panel_screen[i] = g && k_screens[i] == HT_APPLE ? g->apple : k_screens[i];
        L->panel[i] = bx(x, py, t->panel_w, t->panel_h);
        L->panel_ramp[i] = bx(x, ry, t->panel_w, rh);
    }
    push(t, (sect_t){ .y0 = py, .rows = t->panel_h, .idx = 0, .begin = begin_panels, .fill = fill_panels, .end = end_panels });
    push(t, (sect_t){ .y0 = ry, .rows = rh, .idx = 1, .begin = begin_panels, .fill = fill_panels, .end = end_panels });
    y = ry + rh + S(t, 32);
    text(t, 0, y, "The ticked screen prints your pages. Change it in the printer's settings.", 100, 0);
    y += GH_ + GAP_;

    // Alignment: a ruler 8 in long, numbered, and four squares an inch each way with captions.
    y = heading(t, y, "Alignment");
    const uint32_t rul_h = S(t, RULER_H);
    push(t, (sect_t){ .y0 = y, .rows = rul_h, .fill = fill_ruler });
    L->ruler = bx(0, y, W_, rul_h);
    for (uint32_t i = 0; i <= 8; i++) {
        char d[2] = { (char)('0' + i), '\0' };
        text(t, i < 8 ? S(t, i * 360) : W_ - CELL_, y + rul_h + S(t, 12), d, 1, 0);
    }
    y += rul_h + S(t, 12) + GH_ + S(t, 72);
    const uint32_t sq = S(t, SQ);
    push(t, (sect_t){ .y0 = y, .rows = sq, .fill = fill_squares });
    for (int i = 0; i < 4; i++) L->square[i] = bx(S(t, (uint32_t)i * 720), y, sq, sq);
    y += sq + S(t, 28);
    static const char *const caps[2][4] = {
        { "One inch each way", "Solid black", "Straight lines", "Even gray" },
        { "Measure it with a ruler", "White line: a nozzle is out", "The head is aligned", "The paper feeds evenly" },
    };
    const char *const (*cap)[4] = g && g->captions ? g->captions : caps;
    for (int r = 0; r < 2; r++, y += LINE_)
        for (int i = 0; i < 4; i++) text(t, S(t, (uint32_t)i * 720), y, cap[r][i], 30, 0);
    y += S(t, GAP - LINE + 20);

    // The footer.
    rule(t, y);
    y += S(t, 3) + S(t, 24);
    L->footer = bx(0, y, W_, GH_);
    text(t, 0, y, "AirWriter by Wells Workshop    wells.ee    github.com/wr/airwriter", 100, 0);

    L->first_ink = (uint16_t)t->lo;
    L->last_ink = (uint16_t)t->hi;
    L->colour = t->colour;
}

// The page's sizes at its dpi, then its plan. A geometry this page cannot draw (a dpi past
// 360, which every buffer is sized for, or a letterhead icon wider than the StyleWriter's)
// leaves the plan full: no page.
static void setup(tp_t *t, const sw_tp_info_t *i)
{
    const sw_tp_geom_t *g = i->geom;
    t->info = i;
    t->geom = g;
    t->art = g ? g->art : &k_sw_art;
    t->model = i->model;
    t->colour = !g && i->colour_cart && sw_spec(i->model)->colour;
    t->screen = i->screen == HT_APPLE_LW ? HT_APPLE : i->screen;
    t->gain = i->gain;
    t->dpi = g ? g->dpi : 360;
    if (t->dpi < 72 || t->dpi > 360 || !t->art) { t->full = 1; return; }
    t->w = S(t, W);
    t->px = S(t, PX);
    t->cell = 6 * t->px;
    t->gh = 7 * t->px;
    t->icon_s = S(t, ICON_S);
    t->icon_w = (uint32_t)t->art->icon_w * t->icon_s;
    if (t->icon_w > ICON_MAX || t->icon_w % 8 || (uint32_t)t->art->sub_w > t->w) { t->full = 1; return; }
    t->title_x = t->icon_w + S(t, TITLE_DX);
    t->title_y = S(t, TITLE_Y);
    t->sub_y = S(t, SUB_Y);
    t->head_h = t->sub_y + t->art->sub_h;
    t->photo_w = S8(t, PHOTO_W);
    t->photo_h = S(t, PHOTO_H);
    t->side_x = S8(t, SIDE_X);
    t->side_w = t->w - t->side_x;
    t->step_w = S8(t, STEP_W);
    t->step_dx = S8(t, STEP_DX);
    t->panel_w = S8(t, PANEL_W);
    t->panel_h = S(t, PANEL_H);
    t->panel_dx = S8(t, PANEL_DX);
    plan(t);
}

void sw_testpage_layout(const sw_tp_info_t *i, sw_tp_layout_t *out)
{
    memset(out, 0, sizeof *out);
    if (!i || (!i->geom && !sw_spec(i->model))) return;
    tp_t *t = big_calloc(1, sizeof *t);
    if (!t) return;
    setup(t, i);
    *out = t->lay;
    free(t);
}

// ------------------------------------------------------------------ the page

int sw_testpage(const sw_tp_info_t *i, const rip_stage_t *st)
{
    const sw_tp_geom_t *g = i ? i->geom : NULL;
    const sw_spec_t *sp = i && !g ? sw_spec(i->model) : NULL;
    if (!i || (!g && !sp)) return SW_TP_NOMEM;
    int colour = !g && i->colour_cart && sp->colour;
    if (!st || !st->page_begin || !st->row || !st->page_end || (colour && !st->ink_row))
        return SW_TP_STAGE;                  // a stage that cannot take this page's rows
    const uint32_t pw = g ? g->sheet_w : sw_printable_w(i->model, SW_TP_SHEET_W), rb = (pw + 7) / 8;
    const uint32_t sheet_h = g ? g->sheet_h : SW_TP_SHEET_H, mt = g ? g->margin_t : sp->margin_t;
    if (pw < scale(g ? g->dpi : 360, W)) return SW_TP_NOMEM;   // (no model has less than 8 in across)

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

    if (rc == SW_TP_DONE && st->page_begin(st->ctx, 0, pw, sheet_h)) rc = SW_TP_STAGE;
    for (uint32_t sy = 0; rc == SW_TP_DONE && sy < sheet_h; sy++) {
        if ((sy & 31) == 0 && i->stop && i->stop(i->stop_ctx)) { rc = SW_TP_STOPPED; break; }
        memset(rows, 0, (size_t)IW_INKS * rb);
        // The layout starts under the engine's top margin; the margins are blank rows.
        if (sy >= mt && sy - mt <= t->hi) {
            uint32_t y = sy - mt;
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
