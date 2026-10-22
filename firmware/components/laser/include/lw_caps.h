// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AirPrint printer a queried LaserWriter becomes (W-1082, laser printers
// design, "The AirPrint printer"): URF RS300 and W8 only, the one paper size
// it reported, margins from its imageable area rounded outward to whole
// 1/100 mm, and a printer-uuid of its own.
#pragma once
#include <stdint.h>
#include "printer_caps.h"
#include "lw_query.h"

typedef struct {
    char           make_model[72];
    char           device_id[112];
    printer_caps_t caps;        // its strings point into this struct: fill it in place
} lw_caps_t;

void     lw_caps_fill(lw_caps_t *c, const lw_info_t *i);
uint32_t lw_paper_h_pt(lw_page_t p);     // 792, 842, 1008

// urn:uuid:41575254-4c57-<epoch>-<hash>-<mac>: the hash is FNV-1a 32 of the
// shared printer's NBP name (a Pascal string, MacRoman), folded to 16 bits.
// Sharing another LaserWriter is another printer to iOS, which it is; the
// ImageWriter's UUID never moves.
void     lw_uuid(char out[46], const uint8_t mac[6], uint16_t epoch, const uint8_t *pascal_name);
