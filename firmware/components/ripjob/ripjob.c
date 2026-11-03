// SPDX-License-Identifier: GPL-3.0-or-later
#include "ripjob.h"
#include "bigmem.h"
#include "resample.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

// The page's width at rip dpi names a sheet of the caps' list when it is within
// this many dots of its page_w: a phone rounds an A4 page by a dot or two.
#define MEDIA_SLACK 2

typedef struct {
    const ripjob_cfg_t *cfg;
    ht_t      ht;
    iw_t      iw;
    uint8_t  *bits;
    uint32_t  dst_w;
    uint32_t  y;
    // A colour page (W-1064): the ink screens (ink.h) and a 1bpp row per ink.
    iwc_t    *iwc;
    ink_screens_t inks;
    uint8_t  *ibits[IW_INKS];
    uint8_t  *gray;          // an RGB row as gray, for a black ribbon
    uint32_t  skip;          // rows of this page still to drop (top_margin < 0)
} pipe_t;

static void say(const ripjob_cfg_t *cfg, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void say(const ripjob_cfg_t *cfg, const char *fmt, ...)
{
    if (!cfg->log) return;
    char msg[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cfg->log(cfg->log_ctx, msg);
}

// One resampled row: halftone it, hand it to the printer stage, and show it to
// whoever asked for rows.
static void on_row(void *ctx, const uint8_t *row, uint32_t w)
{
    pipe_t *p = (pipe_t *)ctx;
    ht_row(&p->ht, row, p->bits);
    if (p->cfg->row) p->cfg->row(p->cfg->row_ctx, p->bits, w, p->y);
    if (p->cfg->stage) p->cfg->stage->row(p->cfg->stage->ctx, p->bits);
    else if (p->skip) p->skip--;
    else iw_row(&p->iw, p->bits);
    p->y++;
}

// One resampled RGB row of a colour page: split it into inks, halftone each,
// and hand the four rows to the colour strip writer, or to a stage that takes them.
static void on_rgb_row(void *ctx, const uint8_t *row, uint32_t w)
{
    pipe_t *p = (pipe_t *)ctx;
    ink_screens_row(&p->inks, row, p->ibits);
    const uint8_t *const *bits = (const uint8_t *const *)p->ibits;
    if (p->cfg->ink_row) p->cfg->ink_row(p->cfg->row_ctx, bits, w, p->y);
    if (p->cfg->stage) p->cfg->stage->ink_row(p->cfg->stage->ctx, bits);
    else if (p->skip) p->skip--;
    else iwc_row(p->iwc, bits);
    p->y++;
}

// Apple's drivers start a page on the screen's second row (apple_tables.h).
static void apple_page_start(ht_t *h)
{
    ht_set_phase(h, h->phase_x, h->phase_y + 1, h->invert);
}

// The sheet of the caps' list this page's width names, or the first.
static const printer_media_t *pick_media(const printer_caps_t *caps, uint32_t page_w)
{
    for (size_t i = 0; i < caps->n_media; i++) {
        int32_t d = (int32_t)page_w - (int32_t)caps->media_list[i].page_w;
        if (d >= -MEDIA_SLACK && d <= MEDIA_SLACK) return &caps->media_list[i];
    }
    return &caps->media_list[0];
}

// URF colour spaces that are plain RGB: 1 sRGB, 3 AdobeRGB, 5 device RGB.
// 2 (CIELab) also has three colours and is not.
static int is_rgb(const urf_page_t *pg)
{
    return pg->bits_per_pixel == 24 && pg->num_colors == 3 &&
           (pg->colorspace == 1 || pg->colorspace == 3 || pg->colorspace == 5);
}

int ripjob_run(urf_t *u, const ripjob_cfg_t *cfg, ripjob_stats_t *st)
{
    // Off the stack: its ImageWriter encoder and ink screens are ~3.8 KB, and
    // the rip task has 7 KB of internal RAM for its stack (W-1082 bench: a
    // LaserWriter's job left 372 bytes of it).
    pipe_t *p = big_calloc(1, sizeof *p);
    if (!p) return -1;
    p->cfg = cfg;
    const printer_caps_t *caps = cfg->caps ? cfg->caps : &printer_caps_imagewriter;
    const rip_stage_t *stage = cfg->stage;
    uint32_t dpi_x = caps->rip_dpi_x, dpi_y = caps->rip_dpi_y, max = caps->max_dots;

    int pages = 0, rc;

    while ((rc = urf_next_page(u)) == URF_OK) {
        int rgb = is_rgb(&u->page);
        if (!rgb && (u->page.bits_per_pixel != 8 || u->page.num_colors != 1)) {
            say(cfg, "page %d is %u bpp / %u colours (space %u); only 8-bit grayscale "
                     "and 24-bit RGB are supported",
                pages + 1, u->page.bits_per_pixel, u->page.num_colors, u->page.colorspace);
            rc = -1;
            break;
        }
        // With a stage, colour is the stage's to take: four rows a pixel row, or none.
        int colour = rgb && cfg->colour && (!stage || stage->ink_row);

        uint32_t res    = u->page.res_x ? u->page.res_x : dpi_x;
        uint32_t full_w = (uint32_t)((uint64_t)u->page.width * dpi_x / res);

        // SPEC.md 3.5 declares a 0.25in left/right margin to iOS, so the
        // outermost res/4 source pixels per side are white by construction.
        // A page wider than the ImageWriter's 8in / 1152 dots is US Letter
        // (8.5in) arriving at full bleed; crop the margin instead of
        // squashing the whole page down to fit. A printer with no such
        // margin (W-1082) keeps the page's left edge where it is and loses
        // what lies past its widest row: the paper is the page, top-left.
        uint32_t src_w = u->page.width, crop_left = 0, crop_per_side = 0;
        int clipped = 0;
        // A printer with sheets (W-1103) keeps its engine's printable area of the
        // sheet the page's width names: rip_w dots from rip_left, or the page's own
        // width when that is less, so a short page is never padded and a page the
        // engine cannot print wide never reaches it.
        uint32_t limit = max;
        int sheeted = caps->n_media != 0;
        if (sheeted) {
            const printer_media_t *md = pick_media(caps, full_w);
            uint32_t left = md->rip_left < full_w ? md->rip_left : 0;
            uint32_t keep = md->rip_w < full_w - left ? md->rip_w : full_w - left;
            crop_left = (uint32_t)((uint64_t)left * res / dpi_x);
            src_w = (uint32_t)((uint64_t)keep * res / dpi_x);
            if (src_w > u->page.width - crop_left) src_w = u->page.width - crop_left;
            if (!src_w) src_w = 1;
            crop_per_side = crop_left;
            limit = md->rip_w;
        } else if (max && full_w > max) {
            if (caps->crop_margin) {
                uint32_t shortfall_dst  = full_w - max;
                uint32_t shortfall_src  = (uint32_t)((uint64_t)shortfall_dst * res / dpi_x);
                uint32_t max_per_side   = res / 4;             // 0.25in in source px
                crop_per_side = shortfall_src / 2;
                if (crop_per_side > max_per_side) crop_per_side = max_per_side;
                crop_left = crop_per_side;
                src_w = u->page.width - 2 * crop_per_side;
            } else {
                src_w = (uint32_t)((uint64_t)max * res / dpi_x);
                clipped = 1;
            }
        }

        uint32_t dst_w = (uint32_t)((uint64_t)src_w * dpi_x / res);
        uint32_t dst_h = (uint32_t)((uint64_t)u->page.height * dpi_y / res);

        int still_wide = 0;
        if (limit && dst_w > limit) {
            dst_w = limit;
            still_wide = 1;
        }
        // A sheeted printer's crop is how its pages are meant to go: nothing to say.
        if (!sheeted && clipped) {
            say(cfg, "page %d width %u dots exceeds %u; clipped on the right",
                pages + 1, (unsigned)full_w, (unsigned)max);
        } else if (!sheeted && (crop_per_side || still_wide)) {
            say(cfg, "page %d width %u dots exceeds %u; cropped %u source px per side "
                     "(0.25in margin)%s",
                pages + 1, (unsigned)full_w, (unsigned)max,
                (unsigned)crop_per_side, still_wide ? ", still scaled to fit" : "");
        }
        if (!dst_w || !dst_h) { say(cfg, "empty page"); rc = -1; break; }

        p->dst_w = dst_w;
        p->y     = 0;
        p->skip  = !stage && cfg->top_margin < 0 ? (uint32_t)(-cfg->top_margin / 2) : 0;
        if (st) { st->dst_w = dst_w; st->dst_h = dst_h; st->crop_per_side = crop_per_side; }

        if (cfg->page && cfg->page(cfg->page_ctx, pages, dst_w, dst_h)) {
            say(cfg, "page %d refused by the caller", pages + 1);
            rc = -1;
            break;
        }
        if (stage && stage->page_begin(stage->ctx, pages, dst_w, dst_h)) {
            say(cfg, "page %d refused by the output stage", pages + 1);
            rc = -1;
            break;
        }

        /* From here on, every failure jumps to page_cleanup instead of
         * breaking directly, so nothing allocated for this page is ever
         * leaked and ht_free/rs_free only run on state that was actually
         * initialised. */
        int  ht_ready = 0, inks_ready = 0, rs_ready = 0;
        rs_t rs;

        if (colour) {
            if (!stage) {
                p->iwc = big_calloc(1, sizeof *p->iwc);
                if (!p->iwc) { rc = -1; goto page_cleanup; }
            }
            for (int i = 0; i < IW_INKS; i++)
                if (!(p->ibits[i] = big_calloc((dst_w + 7) / 8, 1))) { rc = -1; goto page_cleanup; }
            if (ink_screens_init(&p->inks, cfg->dither, cfg->gain, dst_w)) {
                rc = -1; goto page_cleanup;
            }
            inks_ready = 1;
            if (cfg->tone) ink_screens_set_tone(&p->inks, cfg->tone);
            if (cfg->dither == HT_APPLE)
                for (int i = 0; i < IW_INKS; i++) apple_page_start(&p->inks.ht[i]);
            if (!stage) {
                iwc_init(p->iwc, dst_w, cfg->out, cfg->out_ctx);
                iwc_begin_page(p->iwc);
                // The margin's blank rows: ibits are still all zeros here.
                for (int r = 0; r < cfg->top_margin / 2; r++)
                    iwc_row(p->iwc, (const uint8_t *const *)p->ibits);
            }
            if (rs_init_n(&rs, src_w, u->page.height, dst_w, dst_h, 3, on_rgb_row, p)) {
                rc = -1; goto page_cleanup;
            }
        } else {
            p->bits = big_calloc((dst_w + 7) / 8, 1);
            if (!p->bits) { rc = -1; goto page_cleanup; }
            if (rgb && !(p->gray = big_malloc(src_w))) { rc = -1; goto page_cleanup; }

            if (ht_init(&p->ht, cfg->dither, dst_w)) { rc = -1; goto page_cleanup; }
            ht_ready = 1;
            ht_set_gain(&p->ht, cfg->gain);
            if (cfg->tone) ht_set_tone(&p->ht, cfg->tone);
            if (cfg->dither == HT_APPLE) {
                // One plane of gray is the black ink's: its placement, not the colour inks'.
                ink_screen_phase(&p->ht, IW_BLACK);
                apple_page_start(&p->ht);
            }
            if (!stage) {
                iw_init(&p->iw, dst_w, cfg->out, cfg->out_ctx);
                iw_begin_page(&p->iw);
                for (int r = 0; r < cfg->top_margin / 2; r++) iw_row(&p->iw, p->bits);   // still zeros
            }

            if (rs_init(&rs, src_w, u->page.height, dst_w, dst_h, on_row, p)) {
                rc = -1; goto page_cleanup;
            }
        }
        rs_ready = 1;

        {
            const uint8_t *row;
            int r;
            // One byte per pixel for gray and three for RGB, so the crop is
            // that many bytes per pixel in.
            while ((r = urf_next_row(u, &row)) == URF_OK) {
                if (!rgb) {
                    rs_push(&rs, row + crop_left);
                } else if (colour) {
                    rs_push(&rs, row + crop_left * 3);
                } else {
                    ink_gray(row + crop_left * 3, src_w, p->gray);
                    rs_push(&rs, p->gray);
                }
            }
            if (r < 0) {
                say(cfg, "page %d incomplete, stream is truncated", pages + 1);
                rc = r;
            } else if (stage) {
                if (stage->page_end(stage->ctx)) {
                    say(cfg, "page %d: the output stage failed", pages + 1);
                    rc = -1;
                }
            } else if (colour) {
                iwc_end_page(p->iwc);
            } else {
                iw_end_page(&p->iw);
            }
        }

page_cleanup:
        if (rs_ready) rs_free(&rs);
        if (ht_ready) ht_free(&p->ht);
        free(p->bits);
        p->bits = NULL;
        free(p->gray);
        p->gray = NULL;
        // A colour setup that failed part way: free() takes NULL, and the
        // screens free only once made (ink_screens_init cleans up itself).
        if (inks_ready) ink_screens_free(&p->inks);
        for (int i = 0; i < IW_INKS; i++) {
            free(p->ibits[i]);
            p->ibits[i] = NULL;
        }
        free(p->iwc);
        p->iwc = NULL;

        pages++;
        if (rc < 0) break;
    }

    free(p);
    if (st) st->pages = pages;
    return rc < 0 ? rc : 0;
}
