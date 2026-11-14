// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ImageWriter's test page geometry (testpage.h, W-1140): the page at the printer's 144 x 72,
// 8 in across from a quarter inch in, as a phone's page is cropped for it, on an 11 in sheet whose
// first inch is above where printing starts (the top margin setting drops it, iw_stage.h). In color
// on the color ribbon; in gray on the black ribbon, a ribbon it could not read, or the original
// ImageWriter, which never says.
#pragma once
#include "testpage.h"
#include "imagewriter.h"

// model: "ImageWriter II", "ImageWriter LQ" or "ImageWriter" (the original, no color); icon: its
// AirPrint icon's name (pr_icon). 0, or -1 with no model.
int tp_iw_geom(const char *model, const char *icon, iw_ribbon_t ribbon, tp_geom_t *g);
