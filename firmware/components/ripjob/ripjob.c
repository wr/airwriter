// SPDX-License-Identifier: GPL-3.0-or-later
#include "ripjob.h"
#include "resample.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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
    if (p->skip) p->skip--;
    else iw_row(&p->iw, p->bits);
    p->y++;
}

// One resampled RGB row of a colour page: split it into inks, halftone each,
// and hand the four rows to the colour strip writer.
static void on_rgb_row(void *ctx, const uint8_t *row, uint32_t w)
{
    pipe_t *p = (pipe_t *)ctx;
    ink_screens_row(&p->inks, row, p->ibits);
    const uint8_t *const *bits = (const uint8_t *const *)p->ibits;
    if (p->cfg->ink_row) p->cfg->ink_row(p->cfg->row_ctx, bits, w, p->y);
    if (p->skip) p->skip--;
    else iwc_row(p->iwc, bits);
    p->y++;
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
    pipe_t p;
    memset(&p, 0, sizeof p);
    p.cfg = cfg;

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
        int colour = rgb && cfg->colour;

        uint32_t res    = u->page.res_x ? u->page.res_x : RIPJOB_DPI_X;
        uint32_t full_w = (uint32_t)((uint64_t)u->page.width * RIPJOB_DPI_X / res);

        // SPEC.md 3.5 declares a 0.25in left/right margin to iOS, so the
        // outermost res/4 source pixels per side are white by construction.
        // A page wider than the printer's 8in / 1152 dots is US Letter
        // (8.5in) arriving at full bleed; crop the margin instead of
        // squashing the whole page down to fit.
        uint32_t src_w = u->page.width, crop_left = 0, crop_per_side = 0;
        if (full_w > IW_MAX_DOTS) {
            uint32_t shortfall_dst  = full_w - IW_MAX_DOTS;
            uint32_t shortfall_src  = (uint32_t)((uint64_t)shortfall_dst * res / RIPJOB_DPI_X);
            uint32_t max_per_side   = res / 4;             // 0.25in in source px
            crop_per_side = shortfall_src / 2;
            if (crop_per_side > max_per_side) crop_per_side = max_per_side;
            crop_left = crop_per_side;
            src_w = u->page.width - 2 * crop_per_side;
        }

        uint32_t dst_w = (uint32_t)((uint64_t)src_w * RIPJOB_DPI_X / res);
        uint32_t dst_h = (uint32_t)((uint64_t)u->page.height * RIPJOB_DPI_Y / res);

        int still_wide = 0;
        if (dst_w > IW_MAX_DOTS) {
            dst_w = IW_MAX_DOTS;
            still_wide = 1;
        }
        if (crop_per_side || still_wide) {
            say(cfg, "page %d width %u dots exceeds %u; cropped %u source px per side "
                     "(0.25in margin)%s",
                pages + 1, (unsigned)full_w, (unsigned)IW_MAX_DOTS,
                (unsigned)crop_per_side, still_wide ? ", still scaled to fit" : "");
        }
        if (!dst_w || !dst_h) { say(cfg, "empty page"); rc = -1; break; }

        p.dst_w = dst_w;
        p.y     = 0;
        p.skip  = cfg->top_margin < 0 ? (uint32_t)(-cfg->top_margin / 2) : 0;
        if (st) { st->dst_w = dst_w; st->dst_h = dst_h; st->crop_per_side = crop_per_side; }

        if (cfg->page && cfg->page(cfg->page_ctx, pages, dst_w, dst_h)) {
            say(cfg, "page %d refused by the caller", pages + 1);
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
            p.iwc = calloc(1, sizeof *p.iwc);
            if (!p.iwc) { rc = -1; goto page_cleanup; }
            for (int i = 0; i < IW_INKS; i++)
                if (!(p.ibits[i] = calloc((dst_w + 7) / 8, 1))) { rc = -1; goto page_cleanup; }
            if (ink_screens_init(&p.inks, cfg->dither, cfg->gain, dst_w)) {
                rc = -1; goto page_cleanup;
            }
            inks_ready = 1;
            iwc_init(p.iwc, dst_w, cfg->out, cfg->out_ctx);
            iwc_begin_page(p.iwc);
            // The margin's blank rows: ibits are still all zeros here.
            for (int r = 0; r < cfg->top_margin / 2; r++)
                iwc_row(p.iwc, (const uint8_t *const *)p.ibits);
            if (rs_init_n(&rs, src_w, u->page.height, dst_w, dst_h, 3, on_rgb_row, &p)) {
                rc = -1; goto page_cleanup;
            }
        } else {
            p.bits = calloc((dst_w + 7) / 8, 1);
            if (!p.bits) { rc = -1; goto page_cleanup; }
            if (rgb && !(p.gray = malloc(src_w))) { rc = -1; goto page_cleanup; }

            if (ht_init(&p.ht, cfg->dither, dst_w)) { rc = -1; goto page_cleanup; }
            ht_ready = 1;
            ht_set_gain(&p.ht, cfg->gain);
            iw_init(&p.iw, dst_w, cfg->out, cfg->out_ctx);
            iw_begin_page(&p.iw);
            for (int r = 0; r < cfg->top_margin / 2; r++) iw_row(&p.iw, p.bits);   // still zeros

            if (rs_init(&rs, src_w, u->page.height, dst_w, dst_h, on_row, &p)) {
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
                    ink_gray(row + crop_left * 3, src_w, p.gray);
                    rs_push(&rs, p.gray);
                }
            }
            if (r < 0) {
                say(cfg, "page %d incomplete, stream is truncated", pages + 1);
                rc = r;
            } else if (colour) {
                iwc_end_page(p.iwc);
            } else {
                iw_end_page(&p.iw);
            }
        }

page_cleanup:
        if (rs_ready) rs_free(&rs);
        if (ht_ready) ht_free(&p.ht);
        free(p.bits);
        p.bits = NULL;
        free(p.gray);
        p.gray = NULL;
        // A colour setup that failed part way: free() takes NULL, and the
        // screens free only once made (ink_screens_init cleans up itself).
        if (inks_ready) ink_screens_free(&p.inks);
        for (int i = 0; i < IW_INKS; i++) {
            free(p.ibits[i]);
            p.ibits[i] = NULL;
        }
        free(p.iwc);
        p.iwc = NULL;

        pages++;
        if (rc < 0) break;
    }

    if (st) st->pages = pages;
    return rc < 0 ? rc : 0;
}
