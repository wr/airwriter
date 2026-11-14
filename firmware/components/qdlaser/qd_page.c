// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a QuickDraw LaserWriter's page, screened twice and sent in the one that fits (qd_page.h).
#include "qd_page.h"
#include "bigmem.h"
#include "sw_code.h"
#include <string.h>

struct qd_page {
    qd_page_cfg_t cfg;
    uint32_t budget;
    size_t   rb_max;        // the widest image area's row, in bytes
    int      keeping;       // the record's screen is not Apple's and there is a keep buffer
    int      failed;
    // The page being made.
    const qd_media_t *md;
    int      index;
    uint32_t w;             // the rows' width, dots
    uint32_t top;           // the image area's first page row
    uint32_t y, gy;         // the next row's and the next gray row's page row
    uint32_t rows, krows;   // image rows seen in each screen
    uint32_t sent, kept;    // bytes of codes sent, and kept
    size_t   keep_len;
    int      over, kover;   // past the budget (or the keep buffer)
    int      err;
    ht_t     ht;
    int      ht_ready;
    uint8_t *gbits;         // a gray row in Apple's screen, w dots
    uint8_t *rowbuf;        // a row cut or padded to the image area (rb_max)
    uint8_t *codes;         // sw_code_max(rb_max)
};

ht_algo_t qd_page_rip_screen(const qd_page_cfg_t *cfg)
{
    if (cfg->screen == HT_APPLE || cfg->screen == HT_APPLE_LW || !cfg->keep || !cfg->keep_cap)
        return HT_APPLE_LW;
    return cfg->screen;
}

qd_page_t *qd_page_new(const qd_page_cfg_t *cfg)
{
    const qd_spec_t *sp = qd_spec(cfg->model);
    if (!sp || !cfg->sink.open || !cfg->sink.row || !cfg->sink.print) return NULL;
    size_t rb = 0;
    for (int i = 0; i < sp->n_media; i++)
        if ((size_t)sp->media[i].img_w / 8 > rb) rb = sp->media[i].img_w / 8;
    size_t cmax = sw_code_max(rb);
    qd_page_t *p = big_calloc(1, sizeof *p + rb + cmax);
    if (!p) return NULL;
    p->cfg = *cfg;
    p->budget = cfg->budget ? cfg->budget : sp->budget;
    p->rb_max = rb;
    p->keeping = qd_page_rip_screen(cfg) != HT_APPLE_LW;
    p->rowbuf = (uint8_t *)(p + 1);
    p->codes = p->rowbuf + rb;
    return p;
}

static void page_free(qd_page_t *p)
{
    if (p->ht_ready) ht_free(&p->ht);
    p->ht_ready = 0;
    free(p->gbits);
    p->gbits = NULL;
}

void qd_page_free(qd_page_t *p)
{
    if (!p) return;
    page_free(p);
    free(p);
}

int qd_page_failed(const qd_page_t *p)
{
    return p->failed;
}

// A row of w dots, cut or padded with white to the image area, coded.
static size_t code(qd_page_t *p, const uint8_t *bits)
{
    size_t rb = p->md->img_w / 8, have = (p->w + 7) / 8;
    if (have >= rb) {
        memcpy(p->rowbuf, bits, rb);
    } else {
        memcpy(p->rowbuf, bits, have);
        if (p->w & 7) p->rowbuf[have - 1] &= (uint8_t)(0xFF00 >> (p->w & 7));
        memset(p->rowbuf + have, 0, rb - have);
    }
    return sw_code_row(p->rowbuf, NULL, rb, p->codes);
}

static size_t code_blank(qd_page_t *p)
{
    size_t rb = p->md->img_w / 8;
    memset(p->rowbuf, 0, rb);
    return sw_code_row(p->rowbuf, NULL, rb, p->codes);
}

static void send(qd_page_t *p, size_t n)
{
    p->rows++;
    if (p->over || p->err) return;
    if (p->sent + n > p->budget) {
        p->over = 1;
        return;
    }
    int rc = p->cfg.sink.row(p->cfg.sink.ctx, p->codes, n);
    if (rc) p->err = rc;
    else p->sent += (uint32_t)n;
}

static void keep(qd_page_t *p, size_t n)
{
    p->krows++;
    if (p->kover) return;
    if (p->kept + n > p->budget || p->keep_len + 2 + n > p->cfg.keep_cap) {
        p->kover = 1;
        return;
    }
    uint8_t *k = p->cfg.keep + p->keep_len;
    k[0] = (uint8_t)n;
    k[1] = (uint8_t)(n >> 8);
    memcpy(k + 2, p->codes, n);
    p->keep_len += 2 + n;
    p->kept += (uint32_t)n;
}

static int in_area(const qd_page_t *p, uint32_t y)
{
    return y >= p->top && y - p->top < p->md->img_h;
}

static int st_begin(void *ctx, int index, uint32_t w, uint32_t h)
{
    qd_page_t *p = ctx;
    page_free(p);
    p->md = qd_media_for(p->cfg.model, w, h);
    if (!p->md) return -1;
    p->index = index;
    p->w = w;
    p->top = (p->md->page_h - p->md->img_h) / 2;
    p->y = p->gy = p->rows = p->krows = 0;
    p->sent = p->kept = 0;
    p->keep_len = 0;
    p->over = p->kover = 0;
    p->err = 0;
    if (p->keeping) {
        if (!(p->gbits = big_calloc((w + 7) / 8, 1))) return -1;
        if (ht_init(&p->ht, HT_APPLE_LW, w)) return -1;
        p->ht_ready = 1;
        ht_set_gain(&p->ht, p->cfg.gain);
    }
    return p->cfg.sink.open(p->cfg.sink.ctx, index, p->md);
}

static void st_gray(void *ctx, const uint8_t *gray, uint32_t w)
{
    qd_page_t *p = ctx;
    (void)w;
    // Every row goes through the screen, as ripjob's would, so the tile sits where it would on
    // a page ripped in Apple's screen alone.
    ht_row(&p->ht, gray, p->gbits);
    if (in_area(p, p->gy++) && !p->err) keep(p, code(p, p->gbits));
}

static void st_row(void *ctx, const uint8_t *bits)
{
    qd_page_t *p = ctx;
    if (in_area(p, p->y++) && !p->err) send(p, code(p, bits));
}

static int st_end(void *ctx)
{
    qd_page_t *p = ctx;
    int rc = 0;
    if (p->err) goto out;
    // A page shorter than the image area ends in white.
    while (p->rows < p->md->img_h && !p->err) send(p, code_blank(p));
    while (p->keeping && p->krows < p->md->img_h) keep(p, code_blank(p));
    if (p->err) goto out;
    qd_sink_t *s = &p->cfg.sink;
    if (!p->over) {
        rc = s->print(s->ctx);
        if (!rc && s->note) s->note(s->ctx, p->index, !p->keeping, p->sent, 0);
    } else if (p->keeping && !p->kover) {
        if ((rc = s->open(s->ctx, p->index, p->md))) goto done;
        for (size_t at = 0; at < p->keep_len;) {
            const uint8_t *k = p->cfg.keep + at;
            size_t n = (size_t)k[0] | (size_t)k[1] << 8;
            if ((rc = s->row(s->ctx, k + 2, n))) goto done;
            at += 2 + n;
        }
        rc = s->print(s->ctx);
        if (!rc && s->note) s->note(s->ctx, p->index, 1, p->kept, 0);
    } else {
        p->failed++;
        if (s->note) s->note(s->ctx, p->index, 0, 0, 1);
    }
done:
    page_free(p);
    return rc;
out:
    rc = p->err;
    page_free(p);
    return rc;
}

void qd_page_stage(qd_page_t *p, rip_stage_t *st)
{
    st->page_begin = st_begin;
    st->row        = st_row;
    st->page_end   = st_end;
    st->ctx        = p;
    st->ink_row    = NULL;
    st->gray_row   = p->keeping ? st_gray : NULL;
}
