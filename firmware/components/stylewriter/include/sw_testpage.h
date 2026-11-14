// SPDX-License-Identifier: GPL-3.0-or-later
//
// A StyleWriter's test page geometry (testpage.h, W-1140; W-1103 first drew the page for it): the
// page at 360 dpi across the model's printable width, on a Letter sheet whose margins the stage
// drops (sw_page.h), with the model's own tone curves and ink placement for each screen, as its
// jobs make them. In four inks with a color cartridge in a color model; in gray otherwise.
#pragma once
#include "testpage.h"
#include "sw_model.h"

#define SW_TP_SHEET_W 3060u          // Letter at 360 dpi
#define SW_TP_SHEET_H 3960u

// cart: a color cartridge is in. 0, or -1 for a model it does not know.
int sw_tp_geom(sw_model_t m, int colour_cart, tp_geom_t *g);
