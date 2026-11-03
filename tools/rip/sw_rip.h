// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the two host front ends for a StyleWriter share (W-1103): `rip --printer stylewriter`
// (main.c) and swhost (swhost.c) name the models the same way and fill a ripjob_cfg_t for
// one the same way, so the board's wiring is written once on the host.
#pragma once
#include <string.h>
#include "ripjob.h"
#include "sw_model.h"
#include "sw_page.h"

// "original", "ii", "1200", "1500", "2200", "2400", "2500". 0, or -1 for any other name.
static inline int sw_rip_model(const char *name, sw_model_t *m)
{
    static const struct { const char *name; sw_model_t m; } k[] = {
        { "original", SW_ORIGINAL }, { "ii", SW_II }, { "1200", SW_1200 }, { "1500", SW_C1500 },
        { "2200", SW_C2200 }, { "2400", SW_C2400 }, { "2500", SW_C2500 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcmp(name, k[i].name)) { *m = k[i].m; return 0; }
    return -1;
}

// The printer side of a job's config: the model's caps, the page stage `p` as its output
// stage, Apple's tone curve for the model and the screen (not with `tone` 0), and `colour`,
// which is for RGB pages to print in four inks: a colour job on a colour cartridge, so a
// black cartridge gets its colour pages as gray through the screen.
static inline void sw_rip_cfg(ripjob_cfg_t *cfg, rip_stage_t *stage, sw_page_t *p, sw_model_t m,
                              ht_algo_t algo, int colour, int tone)
{
    sw_page_stage(p, stage);
    cfg->caps   = sw_caps(m);
    cfg->stage  = stage;
    cfg->colour = colour;
    cfg->tone   = tone ? sw_tone(m, algo) : NULL;
}
