// SPDX-License-Identifier: GPL-3.0-or-later
// The ImageWriter's strips as a rip stage (iw_stage.h).
#include "iw_stage.h"
#include "bigmem.h"

#include <stdlib.h>
#include <string.h>

static const uint8_t k_blank[(IW_MAX_DOTS + 7) / 8];

void iw_stage_init(iw_stage_t *s, iw_out_fn out, void *ctx, int colour, int top_margin)
{
    memset(s, 0, sizeof *s);
    s->out = out;
    s->ctx = ctx;
    s->colour = colour;
    s->top_margin = top_margin;
}

void iw_stage_free(iw_stage_t *s)
{
    free(s->iw);
    free(s->iwc);
    s->iw = NULL;
    s->iwc = NULL;
}

// As ripjob's page start: the writer, ESC c and the rest, then the margin's blank rows ahead of
// the page (in whole printer rows, 1/72 in, rounding toward zero), or the rows to drop.
static int page_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    (void)index; (void)h;
    iw_stage_t *s = ctx;
    iw_stage_free(s);
    s->skip = s->top_margin < 0 ? (uint32_t)(-s->top_margin / 2) : 0;
    if (s->colour) {
        if (!(s->iwc = big_calloc(1, sizeof *s->iwc))) return -1;
        iwc_init(s->iwc, w, s->out, s->ctx);
        iwc_begin_page(s->iwc);
        const uint8_t *const blank[IW_INKS] = { k_blank, k_blank, k_blank, k_blank };
        for (int r = 0; r < s->top_margin / 2; r++) iwc_row(s->iwc, blank);
    } else {
        if (!(s->iw = big_calloc(1, sizeof *s->iw))) return -1;
        iw_init(s->iw, w, s->out, s->ctx);
        iw_begin_page(s->iw);
        for (int r = 0; r < s->top_margin / 2; r++) iw_row(s->iw, k_blank);
    }
    return 0;
}

static void row(void *ctx, const uint8_t *bits)
{
    iw_stage_t *s = ctx;
    if (s->skip) s->skip--;
    else if (s->iw) iw_row(s->iw, bits);
}

static void ink_row(void *ctx, const uint8_t *const bits[IW_INKS])
{
    iw_stage_t *s = ctx;
    if (s->skip) s->skip--;
    else if (s->iwc) iwc_row(s->iwc, bits);
}

static int page_end(void *ctx)
{
    iw_stage_t *s = ctx;
    if (s->iwc) iwc_end_page(s->iwc);
    else if (s->iw) iw_end_page(s->iw);
    else return -1;
    iw_stage_free(s);
    return 0;
}

void iw_stage(iw_stage_t *s, rip_stage_t *st)
{
    *st = (rip_stage_t){ .page_begin = page_begin, .row = row, .page_end = page_end,
                         .ink_row = s->colour ? ink_row : NULL, .ctx = s };
}
