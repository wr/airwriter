// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a QuickDraw LaserWriter's test page geometry (qd_testpage.h).
#include "qd_testpage.h"
#include "qd_art.h"

// mkart.py --grid 300x300: "AirWriter", "Test page" and the 300's Chooser icon at 300 dpi.
static const sw_tp_art_t k_art = {
    .title_w = QD_TITLE_W, .title_h = QD_TITLE_H, .sub_w = QD_SUB_W, .sub_h = QD_SUB_H,
    .icon_w = QD_ICON_W, .icon_h = QD_ICON_H,
    .title = &qd_title[0][0], .sub = &qd_sub[0][0], .icon = &qd_icon[0][0], .rgb = qd_icon_rgb,
};

// Under the four squares: what each is, and what a good print of it looks like on a laser.
static const char *const k_captions[2][4] = {
    { "One inch each way", "Solid black", "Straight lines", "Even gray" },
    { "Measure it with a ruler", "Pale or streaky: low toner", "Sharp, even lines", "The paper feeds evenly" },
};

int qd_tp_geom(qd_model_t m, sw_tp_geom_t *g)
{
    const qd_spec_t *sp = qd_spec(m);
    if (!sp || !sp->n_media || !g) return -1;
    const qd_media_t *md = &sp->media[0];
    const uint16_t top = (uint16_t)((md->page_h - md->img_h) / 2);
    *g = (sw_tp_geom_t){
        .dpi = 300, .sheet_w = md->img_w, .sheet_h = md->page_h,
        .margin_l = (uint16_t)((md->page_w - md->img_w) / 2), .margin_t = top,
        .margin_b = (uint16_t)(md->page_h - md->img_h - top),
        .model_name = sp->name, .apple = HT_APPLE_LW, .art = &k_art, .captions = k_captions,
    };
    return 0;
}
