// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a QuickDraw LaserWriter's test page geometry (qd_testpage.h).
#include "qd_testpage.h"
#include "qd_page.h"

#include <stdio.h>

// Under the four squares: what each is, and what a good print of it looks like on a laser.
static const char *const k_captions[2][4] = {
    { "One inch each way", "Solid black", "Straight lines", "Even gray" },
    { "Measure it with a ruler", "Pale or streaky: low toner", "Sharp, even lines", "The paper feeds evenly" },
};

// Each screen's curve, as its jobs have it (qd_tone_for).
static const uint8_t *tone(const void *ctx, ht_algo_t algo)
{
    return qd_tone_for(algo, ctx);
}

int qd_tp_geom(qd_model_t m, tp_geom_t *g, uint8_t toner[256])
{
    const qd_spec_t *sp = qd_spec(m);
    if (!sp || !sp->n_media || !g || !toner) return -1;
    qd_toner_curve(toner);
    const qd_media_t *md = &sp->media[0];
    const uint16_t top = (uint16_t)((md->page_h - md->img_h) / 2);
    *g = (tp_geom_t){
        .dpi_x = 300, .dpi_y = 300,
        .sheet_w = md->img_w, .sheet_h = md->page_h, .left = 0, .top = top,
        .print_w = md->img_w, .print_h = md->img_h,
        .margin_l = (uint16_t)((md->page_w - md->img_w) / 2), .margin_t = top,
        .margin_b = (uint16_t)(md->page_h - md->img_h - top),
        .icon = sp->icon, .colour = 0, .gray_note = NULL,
        .apple = 1, .apple_algo = HT_APPLE_LW, .tone = tone, .tone_ctx = toner, .ink_place = 0,
        .captions = k_captions,
    };
    snprintf(g->model, sizeof g->model, "%s", sp->name);
    return 0;
}
