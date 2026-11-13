// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: a QuickDraw LaserWriter's test page is the StyleWriter's (sw_testpage.h) drawn at
// 300 dpi on Letter's image area: the 300's own icon at the top, Apple's 300 screen in the Apple
// panel, and captions for a laser under the squares. The page goes to the 300's page stage
// (qd_page.h) as a job's rows do, so it is held to the printer's memory the same way.
// Pure C; tools/rip builds it on the host.
#pragma once
#include "sw_testpage.h"
#include "qd_model.h"

// Fills g for model m's first sheet (Letter): rows of its image area's width, the sheet's
// height, the area's place on it. 0, or -1 for a model there is none of.
int qd_tp_geom(qd_model_t m, sw_tp_geom_t *g);
