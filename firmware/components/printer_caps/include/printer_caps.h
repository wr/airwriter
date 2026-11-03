// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a printer can do, in one place (W-1082): what IPP and Bonjour tell
// clients (SPEC.md 3.3, 3.5) and how the rip lays a page out for it. The
// ImageWriter's is a constant; a shared LaserWriter's is filled in from what
// it answered (laser/lw_caps.h). The scope spec's rule stands: this struct,
// and no printer-driver framework beyond it.
#pragma once
#include <stdint.h>
#include <stddef.h>

// One sheet size a printer offers beside others (W-1103: a StyleWriter feeds cut
// sheets, so it lists Letter, A4 and Legal and the phone picks). The margins are
// the engine's, in whole 1/100 mm rounded outward so a client never lays out into
// them. The rip keeps `rip_w` dots of the page from `rip_left`: the page width the
// phone sends at the rip dpi names the entry (`page_w`), and what is kept is the
// engine's printable area, so the page's own left margin never reaches the stage.
typedef struct {
    const char *media;                      // "na_letter_8.5x11in", "iso_a4_210x297mm", "na_legal_8.5x14in"
    int32_t     x, y;                       // 1/100 mm
    int32_t     margin_l, margin_r, margin_t, margin_b;   // 1/100 mm, rounded outward
    uint16_t    page_w;                     // the page's width at rip dpi: 3060, 2976, 3060
    uint16_t    rip_left, rip_w;            // dots kept across
} printer_media_t;

typedef struct {
    // What clients are told.
    const char *make_model;      // printer-make-and-model, TXT ty
    const char *device_id;       // printer-device-id (IEEE 1284)
    const char *urf_rs;          // the URF resolution keyword: "RS72-144", "RS300"
    uint16_t    res_x, res_y;    // printer-resolution-*, dpi
    const char *media;           // the one media keyword: "na_letter_8.5x11in"
    int32_t     media_x, media_y;                        // its size, 1/100 mm
    int32_t     margin_l, margin_r, margin_t, margin_b;  // 1/100 mm
    int32_t     ppm;             // pages-per-minute
    // How the rip lays a page out.
    uint16_t    rip_dpi_x, rip_dpi_y;
    uint32_t    max_dots;        // the widest row the output stage takes; 0 = any
    uint8_t     crop_margin;     // crop up to 1/4 in a side to reach max_dots
    // Sheet sizes (W-1103). With none (n_media 0) the one sheet is the scalar
    // media fields above and the rip crops by crop_margin and max_dots, as the
    // ImageWriter's always has. With a list, the first entry is the default,
    // IPP lists them all, and the rip crops each page by its own entry.
    const printer_media_t *media_list;
    uint8_t     n_media;
} printer_caps_t;

// The ImageWriter II as it has always been advertised: one pass at 144 x 72,
// Letter on the tractor feed, a 0.25 in side margin, 1 in top, 0.5 in bottom.
extern const printer_caps_t printer_caps_imagewriter;
// W-1088: an ImageWriter by model, pr_model_t's numbers: 1 the II (the one
// above), 2 the LQ, 3 the original; anything else the II.
const printer_caps_t *printer_caps_imagewriter_model(int model);

// The URF capability string (SPEC.md 3.3): "V1.4,CP1,<rs>,W8[,SRGB24],DM1,
// IS1,MT1,FN3", SRGB24 when `colour`. Its length, or 0 (and "" when cap > 0)
// when it does not fit.
size_t printer_caps_urf(const printer_caps_t *c, int colour, char *out, size_t cap);
// The same keywords one per value, for urf-supported. Returns the count.
size_t printer_caps_urf_keywords(const printer_caps_t *c, int colour, const char *kw[9]);
