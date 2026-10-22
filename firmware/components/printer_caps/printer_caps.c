// SPDX-License-Identifier: GPL-3.0-or-later
#include "printer_caps.h"
#include <stdio.h>

// Margins per SPEC.md 3.5. The tractor feed means this printer physically
// cannot reach the edges, and declaring borderless would make iOS lay out
// edge-to-edge and silently clip. The top is the inch above where printing
// starts on paper loaded as the Owner's Guide says; the top margin setting
// drops that inch of each page, so what iOS draws below it prints where iOS
// put it. 144 dpi across, 72 down: one pass, SPEC.md 3.8. iOS honoured
// RS72-144 in the capture at fixtures/ios-20260804-232652.urf -- it rendered
// at 144 dpi rather than sending 300 -- so no RS300 fallback is advertised.
const printer_caps_t printer_caps_imagewriter = {
    .make_model  = "Apple ImageWriter II",
    .device_id   = "MFG:Apple;MDL:ImageWriter II;CMD:URF;",
    .urf_rs      = "RS72-144",
    .res_x       = 144, .res_y = 72,
    .media       = "na_letter_8.5x11in",
    .media_x     = 21590, .media_y = 27940,       // 8.5 x 11 in
    .margin_l    = 635, .margin_r = 635,          // 6.35 mm = 0.25 in
    .margin_t    = 2540,                          // 25.4 mm = 1 in
    .margin_b    = 1270,                          // 12.7 mm = 0.5 in
    .ppm         = 1,
    .rip_dpi_x   = 144, .rip_dpi_y = 72,
    .max_dots    = 1152,                          // IW_MAX_DOTS: 8 in at 144 dpi
    .crop_margin = 1,
};

size_t printer_caps_urf_keywords(const printer_caps_t *c, int colour, const char *kw[9])
{
    size_t n = 0;
    kw[n++] = "V1.4";
    kw[n++] = "CP1";
    kw[n++] = c->urf_rs;
    kw[n++] = "W8";
    if (colour) kw[n++] = "SRGB24";
    kw[n++] = "DM1";
    kw[n++] = "IS1";
    kw[n++] = "MT1";
    kw[n++] = "FN3";
    return n;
}

size_t printer_caps_urf(const printer_caps_t *c, int colour, char *out, size_t cap)
{
    const char *kw[9];
    size_t n = printer_caps_urf_keywords(c, colour, kw), len = 0;
    for (size_t i = 0; i < n; i++) {
        int w = snprintf(out + len, cap > len ? cap - len : 0, "%s%s", i ? "," : "", kw[i]);
        if (w < 0 || len + (size_t)w >= cap) {
            if (cap) out[0] = '\0';
            return 0;
        }
        len += (size_t)w;
    }
    return len;
}
