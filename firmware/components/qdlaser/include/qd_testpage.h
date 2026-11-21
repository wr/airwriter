// SPDX-License-Identifier: GPL-3.0-or-later
//
// A QuickDraw LaserWriter's test page geometry (testpage.h; W-1102 first drew the page for it):
// the page at 300 dpi on its first sheet's (Letter's) image area, its own icon, Apple's 300
// screen as the Apple panel, and captions for a laser under the squares. The page goes to the
// 300's page stage (qd_page.h) as a job's rows do, so it is held to the printer's memory the same
// way. Pure C; tools/rip builds it on the host.
#pragma once
#include "testpage.h"
#include "qd_model.h"

// Fills g for model m's first sheet: rows of its image area's width, the sheet's height, the
// area's place on it. Its screens go through the toner curve as a job's do (W-1174, qd_page.h
// qd_tone_for), written to `toner`, which has to last while the page is drawn. 0, or -1 for a
// model there is none of.
int qd_tp_geom(qd_model_t m, tp_geom_t *g, uint8_t toner[256]);
