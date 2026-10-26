// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_page.h"
#include "sw_code.h"
#include "bigmem.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_HEAD 3                 // type, u16 LE length

struct sw_page {
    sw_page_cfg_t cfg;
    const sw_spec_t *spec;
    int       colour_cart;           // the cartridge can print four planes
    uint32_t  max_w;                 // the widest printable row on any sheet the model offers

    // The page in hand.
    int       in_page;
    uint32_t  h, y;                  // rows the page has, rows fed so far
    uint32_t  top, end;              // rows kept: top <= y < end
    uint32_t  pw, rowbytes;          // columns kept across, bytes a plane row takes
    int       colour;                // this page prints four planes; -1 until its first row says
    int       planes;                // 4 or 1

    // The band being collected: band_rows x planes x rowbytes, a row's planes together.
    uint8_t  *band;
    uint32_t  band_y0, band_n;       // printable row of its first row, rows in it
    int       band_ink;              // any ink in it

    // The block being coded, with the frame's head in front: head, then coded rows.
    uint8_t  *blk;
    uint32_t  blk_n, blk_cap;
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

    sw_page_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->cfg = *cfg;
    p->spec = spec;
    p->colour_cart = cfg->colour_cart && spec->colour;
    for (size_t i = 0; i < caps->n_media; i++)
        if (caps->media_list[i].rip_w > p->max_w) p->max_w = caps->media_list[i].rip_w;
    if (!p->max_w || p->max_w > spec->dots_across) p->max_w = spec->dots_across;

    int planes = p->colour_cart ? 4 : 1;
    size_t rowbytes = (p->max_w + 7) / 8;
    p->blk_cap = sw_block_cap(cfg->model, 1);
    uint32_t cap4 = sw_block_cap(cfg->model, 4);
    if (p->colour_cart && cap4 > p->blk_cap) p->blk_cap = cap4;
    p->band = big_malloc((size_t)spec->band_rows * planes * rowbytes);
    p->blk = big_malloc(FRAME_HEAD + p->blk_cap);
    if (!p->band || !p->blk || p->blk_cap < planes * sw_code_max(rowbytes)) {
        sw_page_free(p);
        return NULL;
    }
    return p;
}

void sw_page_free(sw_page_t *p)
{
    if (!p) return;
    free(p->band);
    free(p->blk);
    free(p);
}

// The page's kind goes out before its first rect: a colour page is one whose rows
// come as four planes to a printer that can print them.
static void start_page(sw_page_t *p, int ink_rows)
{
    p->colour = ink_rows && p->colour_cart;
    p->planes = p->colour ? 4 : 1;
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

// The band's rect, then its rows coded into blocks of whole rows. The row before is the
// band's own, whichever block it went out in, so the delta carries across blocks.
static void flush_band(sw_page_t *p)
{
    uint32_t n = p->band_n;
    if (!n) return;
    if (p->band_ink) {
        uint8_t f[FRAME_HEAD + 9] = { SWF_RECT, 9, 0, (uint8_t)(p->colour ? 'c' : 'R') };
        put16(f + 4, p->spec->rect_dx);
        put16(f + 6, p->band_y0);
        put16(f + 8, p->spec->rect_dx + p->pw - 1);
        put16(f + 10, p->band_y0 + n - 1);
        emit(p, f, sizeof f);

        size_t rb = p->rowbytes, room = (size_t)p->planes * sw_code_max(rb);
        for (uint32_t r = 0; r < n; r++) {
            if (p->blk_n && p->blk_n + room > p->blk_cap) flush_block(p);
            for (int k = 0; k < p->planes; k++) {
                const uint8_t *row = p->band + ((size_t)r * p->planes + k) * rb;
                const uint8_t *prev = p->spec->delta && r
                                    ? p->band + ((size_t)(r - 1) * p->planes + k) * rb : NULL;
                p->blk_n += (uint32_t)sw_code_row(row, prev, rb, p->blk + FRAME_HEAD + p->blk_n);
            }
        }
        flush_block(p);
    }
    p->band_y0 += n;
    p->band_n = 0;
    p->band_ink = 0;
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

    size_t rb = p->rowbytes;
    uint8_t *slot = p->band + (size_t)p->band_n * p->planes * rb;
    const uint8_t *src[4] = { c, m, y, k };
    uint8_t any = 0;
    for (int i = 0; i < p->planes; i++) {
        uint8_t *dst = slot + (size_t)i * rb;
        const uint8_t *s = p->planes == 4 ? src[i] : k;
        if (s) memcpy(dst, s, rb); else memset(dst, 0, rb);
        if (p->pw & 7) dst[rb - 1] &= (uint8_t)(0xFF << (8 - (p->pw & 7)));
        for (size_t b = 0; b < rb; b++) any |= dst[b];
    }
    p->band_ink |= any != 0;
    if (++p->band_n == p->spec->band_rows) flush_band(p);
}

static int st_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)index;
    sw_page_t *p = ctx;
    if (!w || !h) return -1;
    p->pw = w < p->max_w ? w : p->max_w;
    p->rowbytes = (p->pw + 7) / 8;
    p->h = h;
    p->y = 0;
    p->top = p->spec->margin_t;
    p->end = h > p->spec->margin_b ? h - p->spec->margin_b : 0;
    p->colour = -1;
    p->planes = 1;
    p->band_y0 = p->band_n = 0;
    p->band_ink = 0;
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
    flush_band(p);                            // the last band, short or not
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
