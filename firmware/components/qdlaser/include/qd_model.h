// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the QuickDraw LaserWriters on the printer port, by model. What each takes from the
// board (its sheets and where a page sits on them, its paper sources, its screen and memory)
// and what AirPrint is told about it. The Personal LaserWriter 300 is the one built; the LS
// and the Select 300 are named, so a person learns what is on the port, and never shared.
#pragma once
#include <stdint.h>
#include "printer_caps.h"

typedef enum { QD_NONE, QD_PLW300, QD_MODELS } qd_model_t;
// What `83 3E` answers: the engine's identity.
enum { QD_ENGINE_PLW300 = 0x29, QD_ENGINE_LS = 0x0D, QD_ENGINE_JP = 0x23 };

// One sheet: Apple's paper and image area (from its driver), and the page a client sends for
// the IPP size at 300 dpi, which may differ by a few dots (A4).
typedef struct {
    const char *media;            // "na_letter_8.5x11in" ...
    uint16_t paper_w, paper_h;    // Apple's paper in dots at 300 dpi (PREC 3, 1/120 in x 2.5, rounded down)
    uint16_t page_w, page_h;      // the page a client sends at 300 dpi for the IPP size
    uint16_t img_w, img_h;        // Apple's image area; img_w a multiple of 8
} qd_media_t;

typedef struct {
    const char *name;             // "Personal LaserWriter 300"
    uint8_t  engine;              // 83 3E's answer
    uint8_t  ctrl;                // 82 00 bits 7-6: 2 (no page RAM pointers)
    uint8_t  page_ram;            // 0
    uint8_t  src_cassette, src_manual;   // 83 4C, 83 4F
    uint8_t  bit_cassette, bit_manual;   // 83 2A: 0x20, 0x40
    uint8_t  density_cmd, density;       // 0xA7, 32
    uint32_t budget;              // bytes of row codes a page may take: 500000
    const uint8_t *screen8, *tone;       // apple_lw_screen8, apple_lw_tone
    const qd_media_t *media; uint8_t n_media;   // Letter first
    const char *icon;             // "plw300"
} qd_spec_t;

// The values 85/86, 87 and 88/89 carry, already negative.
typedef struct { int16_t top, left, width; } qd_setup_t;

const qd_spec_t      *qd_spec(qd_model_t m);                 // NULL for QD_NONE or out of range
qd_model_t            qd_model_from_engine(uint8_t engine);  // QD_PLW300 for 0x29, else QD_NONE
// The printer an engine identity names, for a person: 0x0D the LS, 0x23 none (a Japanese
// model, NULL), 0x29 the 300, any other the Select 300.
const char           *qd_engine_name(uint8_t engine);
// The sheet a page of w dots across (the image width the rip keeps) and h rows is on: the
// sheet whose img_w is w and whose page_h is within 2 of h; else by w alone; else the first.
// NULL for a model with none.
const qd_media_t     *qd_media_for(qd_model_t m, uint32_t w, uint32_t h);
qd_setup_t            qd_setup(const qd_media_t *md);
const printer_caps_t *qd_caps(qd_model_t m);                  // NULL for QD_NONE
