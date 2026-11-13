// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the two host front ends for a QuickDraw LaserWriter share (W-1102): `rip --printer
// plw300` (main.c) and qdhost (qdhost.c) fill a ripjob_cfg_t for one the same way the board
// does, so the screen ripjob uses is the stage's choice in all three.
#pragma once
#include "ripjob.h"
#include "qd_page.h"

// The printer side of a job's config: the model's caps, the page stage `p` as its output stage,
// the screen the stage asks for (Apple's 300 screen when the record's is Apple's or nothing is
// kept), the record's gain. Gray only: the printer is black.
static inline void qd_rip_cfg(ripjob_cfg_t *cfg, rip_stage_t *stage, qd_page_t *p, const qd_page_cfg_t *pc)
{
    qd_page_stage(p, stage);
    cfg->caps   = qd_caps(pc->model);
    cfg->stage  = stage;
    cfg->dither = qd_page_rip_screen(pc);
    cfg->gain   = pc->gain;
    cfg->colour = 0;
    cfg->tone   = NULL;
}
