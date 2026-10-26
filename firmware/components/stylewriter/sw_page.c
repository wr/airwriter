// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_page.h"
#include "sw_code.h"
#include "bigmem.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_HEAD 3                 // type, u16 LE length

// Rows are coded as they arrive; nothing of the page is kept. What the stage holds is
// its own struct, the block being filled and the row before in each plane (to XOR the
// next against), one allocation in all (sw_page_new). A colour 1500's band would have
// been 1.15 MB of raw rows beside the board's 1 MB job buffer in 2 MB of PSRAM.
struct sw_page {
    sw_page_cfg_t cfg;
    const sw_spec_t *spec;
    int       colour_cart;           // the cartridge can print four planes
    uint32_t  max_w;                 // the widest printable row on any sheet the model offers

    // The page in hand.
    int       in_page;
    uint32_t  y;                     // rows fed so far
    uint32_t  top, end;              // rows kept: top <= y < end
    uint32_t  nrows;                 // end - top: the printable rows
    uint32_t  pw, rowbytes;          // columns kept across, bytes a plane row takes
    uint8_t   tail;                  // the mask of a row's last byte: bits past pw are not ink
    int       colour;                // this page prints four planes; -1 until its first row says
    int       planes;                // 4 or 1
    uint32_t  cap;                   // sw_block_cap for this page's planes

    // The band in hand: rows of it still to come (0 between bands), and whether the row
    // before is in it, to code against.
    uint32_t  band_left;
    int       have_prev;

    // The block being filled: the frame's head, then coded rows.
    uint8_t  *blk;
    uint32_t  blk_n;
    uint32_t  blk_cap;               // sw_block_cap for the most planes the stage prints
    uint8_t  *prev;                  // the row before, a plane after another, rowbytes each
    uint8_t  *scratch;               // the row being coded: rowbytes
};

static void emit(sw_page_t *p, const uint8_t *frame, size_t n)
{
    p->cfg.out(p->cfg.ctx, frame, n);
}

static void put16(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
}

sw_page_t *sw_page_new(const sw_page_cfg_t *cfg)
{
    const sw_spec_t *spec = cfg ? sw_spec(cfg->model) : NULL;
    const printer_caps_t *caps = cfg ? sw_caps(cfg->model) : NULL;
    if (!spec || !caps || !cfg->out) return NULL;

    int colour_cart = cfg->colour_cart && spec->colour;
    uint32_t max_w = 0;
    for (size_t i = 0; i < caps->n_media; i++)
        if (caps->media_list[i].rip_w > max_w) max_w = caps->media_list[i].rip_w;
    if (!max_w || max_w > spec->dots_across) max_w = spec->dots_across;

    int planes = colour_cart ? 4 : 1;
    size_t rowbytes = (max_w + 7) / 8;
    uint32_t cap = sw_block_cap(cfg->model, 1);
    uint32_t cap4 = sw_block_cap(cfg->model, 4);
    if (colour_cart && cap4 > cap) cap = cap4;
    if (cap < (uint32_t)planes * sw_code_max(rowbytes)) return NULL;     // not one row to a block

    // One allocation: the struct, then the block (head and data), the previous row of
    // each plane, and the row being coded.
    size_t blk_n = FRAME_HEAD + cap, prev_n = (size_t)planes * rowbytes;
    sw_page_t *p = big_malloc(sizeof *p + blk_n + prev_n + rowbytes);
    if (!p) return NULL;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->spec = spec;
    p->colour_cart = colour_cart;
    p->max_w = max_w;
    p->blk_cap = cap;
    p->blk = (uint8_t *)(p + 1);
    p->prev = p->blk + blk_n;
    p->scratch = p->prev + prev_n;
    return p;
}

void sw_page_free(sw_page_t *p)
{
    free(p);
}

// The page's kind goes out before its first rect: a colour page is one whose rows
// come as four planes to a printer that can print them.
static void start_page(sw_page_t *p, int ink_rows)
{
    p->colour = ink_rows && p->colour_cart;
    p->planes = p->colour ? 4 : 1;
    p->cap = sw_block_cap(p->cfg.model, p->planes);
    uint8_t f[FRAME_HEAD + 1] = { SWF_PAGE, 1, 0, (uint8_t)p->colour };
    emit(p, f, sizeof f);
}

static void flush_block(sw_page_t *p)
{
    if (!p->blk_n) return;
    p->blk[0] = SWF_BLOCK;
    put16(p->blk + 1, p->blk_n);
    emit(p, p->blk, FRAME_HEAD + p->blk_n);
    p->blk_n = 0;
}

// Plane i's row i as the stage keeps it: a copy in the scratch row, with the bits past
// the width cleared, or white when there is none.
static const uint8_t *plane_row(sw_page_t *p, const uint8_t *src)
{
    if (src) {
        memcpy(p->scratch, src, p->rowbytes);
        p->scratch[p->rowbytes - 1] &= p->tail;
    } else {
        memset(p->scratch, 0, p->rowbytes);
    }
    return p->scratch;
}

static int has_ink(const sw_page_t *p, const uint8_t *const src[4])
{
    size_t rb = p->rowbytes;
    for (int i = 0; i < p->planes; i++) {
        if (!src[i]) continue;
        for (size_t b = 0; b + 1 < rb; b++)
            if (src[i][b]) return 1;
        if (src[i][rb - 1] & p->tail) return 1;
    }
    return 0;
}

// A band begins at printable row `ry`: its rect, as tall as a band is or as far as the
// page goes, goes out before any of its rows.
static void begin_band(sw_page_t *p, uint32_t ry)
{
    uint32_t last = p->nrows - 1, b = ry + p->spec->band_rows - 1;
    if (b > last) b = last;
    uint8_t f[FRAME_HEAD + 9] = { SWF_RECT, 9, 0, (uint8_t)(p->colour ? 'c' : 'R') };
    put16(f + 4, p->spec->rect_dx);
    put16(f + 6, ry);
    put16(f + 8, p->spec->rect_dx + p->pw - 1);
    put16(f + 10, b);
    emit(p, f, sizeof f);
    p->band_left = b - ry + 1;
    p->have_prev = 0;
}

// One row of the band, coded into the block: each plane against the same plane of the
// row before (the band's first is raw, and the original takes no delta at all). When the
// most a row can take would pass the block's cap the block goes out first, and the next
// row is coded against the one before it all the same, so the delta carries on.
static void put_row(sw_page_t *p, const uint8_t *const src[4])
{
    size_t rb = p->rowbytes, room = (size_t)p->planes * sw_code_max(rb);
    if (p->blk_n && p->blk_n + room > p->cap) flush_block(p);
    for (int i = 0; i < p->planes; i++) {
        const uint8_t *row = plane_row(p, src[i]);
        uint8_t *prev = p->prev + (size_t)i * rb;
        p->blk_n += (uint32_t)sw_code_row(row, p->spec->delta && p->have_prev ? prev : NULL, rb,
                                          p->blk + FRAME_HEAD + p->blk_n);
        if (p->spec->delta) memcpy(prev, row, rb);
    }
    p->have_prev = 1;
    if (--p->band_left == 0) flush_block(p);
}

// One row in: a plane each of cyan, magenta, yellow and black (NULL: no ink), or for a
// gray page just the black one.
static void take(sw_page_t *p, int ink_rows, const uint8_t *c, const uint8_t *m,
                 const uint8_t *y, const uint8_t *k)
{
    if (!p->in_page) return;
    if (p->colour < 0) start_page(p, ink_rows);
    uint32_t row = p->y++;
    if (row < p->top || row >= p->end) return;

    const uint8_t *src[4] = { c, m, y, k };
    if (p->planes == 1) src[0] = k;
    if (!p->band_left) {
        if (!has_ink(p, src)) return;               // between bands: nothing goes out
        begin_band(p, row - p->top);
    }
    put_row(p, src);
}

static int st_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)index;
    sw_page_t *p = ctx;
    if (!w || !h) return -1;
    p->pw = w < p->max_w ? w : p->max_w;
    p->rowbytes = (p->pw + 7) / 8;
    p->tail = p->pw & 7 ? (uint8_t)(0xFF << (8 - (p->pw & 7))) : 0xFF;
    p->y = 0;
    p->top = p->spec->margin_t;
    p->end = h > p->spec->margin_b ? h - p->spec->margin_b : 0;
    p->nrows = p->end > p->top ? p->end - p->top : 0;
    p->colour = -1;
    p->planes = 1;
    p->band_left = 0;
    p->have_prev = 0;
    p->blk_n = 0;
    p->in_page = 1;
    return 0;
}

static void st_row(void *ctx, const uint8_t *bits)
{
    take(ctx, 0, NULL, NULL, NULL, bits);
}

static void st_ink_row(void *ctx, const uint8_t *const bits[IW_INKS])
{
    take(ctx, 1, bits[IW_CYAN], bits[IW_MAGENTA], bits[IW_YELLOW], bits[IW_BLACK]);
}

static int st_end(void *ctx)
{
    sw_page_t *p = ctx;
    if (!p->in_page) return -1;
    // A page that ends with rows of its band still to come (ripjob gives every row, so
    // this is a stage fed short) is finished with white ones, for the rect has said how
    // many there are.
    static const uint8_t *const white[4] = { NULL, NULL, NULL, NULL };
    while (p->band_left) put_row(p, white);
    if (p->colour < 0) start_page(p, 0);      // a page that was given no rows at all
    static const uint8_t f[FRAME_HEAD] = { SWF_PAGE_END, 0, 0 };
    emit(p, f, sizeof f);
    p->in_page = 0;
    return 0;
}

void sw_page_stage(sw_page_t *p, rip_stage_t *st)
{
    st->page_begin = st_begin;
    st->row        = st_row;
    st->page_end   = st_end;
    st->ctx        = p;
    st->ink_row    = st_ink_row;
}
