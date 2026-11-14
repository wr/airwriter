// SPDX-License-Identifier: GPL-3.0-or-later
// The ImageWriter's test page geometry (tp_iw.h).
#include "tp_iw.h"

#include <stdio.h>
#include <string.h>

int tp_iw_geom(const char *model, const char *icon, iw_ribbon_t ribbon, tp_geom_t *g)
{
    if (!model || !g) return -1;
    // The original ImageWriter has no color ribbon and never says which ribbon is in.
    const int can_colour = strcmp(model, "ImageWriter") != 0;
    *g = (tp_geom_t){
        .dpi_x = 144, .dpi_y = 72,
        .sheet_w = IW_MAX_DOTS, .sheet_h = 11 * 72, .left = 0, .top = 72,
        .print_w = IW_MAX_DOTS, .print_h = 11 * 72 - 72 - 36,
        .margin_l = 36, .margin_t = 72, .margin_b = 36,
        .icon = icon,
        .colour = can_colour && ribbon == IW_RIBBON_COLOUR,
        .gray_note = !can_colour ? NULL
                   : ribbon == IW_RIBBON_BLACK
                   ? "The black ribbon is in. With the color ribbon in, this part prints in color."
                   : ribbon == IW_RIBBON_UNKNOWN
                   ? "AirWriter could not tell which ribbon is in, so this part prints in gray." : NULL,
        .apple = 0, .apple_algo = HT_APPLE, .tone = NULL, .ink_place = 1, .captions = NULL,
    };
    snprintf(g->model, sizeof g->model, "%s%s", model,
             !can_colour ? "" : ribbon == IW_RIBBON_COLOUR ? ", color ribbon"
             : ribbon == IW_RIBBON_BLACK ? ", black ribbon" : "");
    return 0;
}
