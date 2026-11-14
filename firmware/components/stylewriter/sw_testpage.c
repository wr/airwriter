// SPDX-License-Identifier: GPL-3.0-or-later
// A StyleWriter's test page geometry (sw_testpage.h).
#include "sw_testpage.h"

#include <stdint.h>
#include <stdio.h>

// Under the four squares: what each is, and what a good print of it looks like on an inkjet.
static const char *const k_captions[2][4] = {
    { "One inch each way", "Solid black", "Straight lines", "Even gray" },
    { "Measure it with a ruler", "White line: a nozzle is out", "The head is aligned", "The paper feeds evenly" },
};

static const uint8_t *tone(const void *ctx, ht_algo_t algo)
{
    return sw_tone((sw_model_t)(uintptr_t)ctx, algo);
}

// Its AirPrint icon's name (printers_core.c pr_icon's).
static const char *icon(sw_model_t m)
{
    switch (m) {
    case SW_ORIGINAL: return "sworiginal";
    case SW_II:       return "sw2";
    case SW_1200:     return "sw1200";
    case SW_C1500:    return "csw1500";
    case SW_C2200:    return "csw2200";
    default:          return "stylewriter";
    }
}

int sw_tp_geom(sw_model_t m, int colour_cart, tp_geom_t *g)
{
    const sw_spec_t *sp = sw_spec(m);
    if (!sp || !g) return -1;
    const uint32_t pw = sw_printable_w(m, SW_TP_SHEET_W);
    *g = (tp_geom_t){
        .dpi_x = 360, .dpi_y = 360,
        .sheet_w = (uint16_t)pw, .sheet_h = SW_TP_SHEET_H, .left = 0, .top = sp->margin_t,
        .print_w = (uint16_t)pw, .print_h = (uint16_t)(SW_TP_SHEET_H - sp->margin_t - sp->margin_b),
        .margin_l = sp->margin_l, .margin_t = sp->margin_t, .margin_b = sp->margin_b,
        .icon = icon(m),
        .colour = colour_cart && sp->colour,
        .gray_note = sp->colour && !colour_cart
                   ? "The black cartridge is in. With a color cartridge in, this part prints in color." : NULL,
        .apple = 1, .apple_algo = HT_APPLE, .tone = tone, .tone_ctx = (const void *)(uintptr_t)m,
        .ink_place = 1, .captions = k_captions,
    };
    snprintf(g->model, sizeof g->model, "%s%s", sp->name,
             !sp->colour ? "" : colour_cart ? ", color cartridge" : ", black cartridge");
    return 0;
}
