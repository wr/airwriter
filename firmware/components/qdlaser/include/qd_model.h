// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the QuickDraw LaserWriters on the printer port, by model. What each takes from the
// board (its sheets and where a page sits on them, its paper sources, its screen and memory)
// and what AirPrint is told about it. The Personal LaserWriter 300 is built and benched;
// W-1145 adds the Personal LaserWriter LS from its own driver, never tried on a printer, and
// W-1146 the LaserWriter Select 300 from its own (LW Select 300 1.0), never tried either.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "printer_caps.h"

typedef enum { QD_NONE, QD_PLW300, QD_PLWLS, QD_SEL300, QD_MODELS } qd_model_t;
// What `83 3E` answers: the engine's identity. Apple's drivers take any other answer for a
// Select 300, which has no number of its own.
enum { QD_ENGINE_PLW300 = 0x29, QD_ENGINE_LS = 0x0D, QD_ENGINE_JP = 0x23 };

// Whose Apple driver a model's conversation follows, where the two differ (qd_session.h):
//   QD_DRV_LW300  LaserWriter 300 1.1: 83 40 in a job, density, 83 2A for paper, the next page
//                 loading while one prints, 83 5D and 84 08 in a jam's recovery, 83 9D's value
//                 shifted with parity, 12-bit row lengths.
//   QD_DRV_LS     Personal LW LS 7.1: none of those. Paper from 83 02 bit 4, one page at a time
//                 (81 00 and the setup before each), 81 00 alone before a jam's pages go again,
//                 83 9D's value as it is, 9-bit row lengths.
//   QD_DRV_SEL    LW Select 300 1.0 (W-1146): the page path by 82 00's answer, page memory or
//                 the 300's; its own sources, density and paper bits (qd_session.h).
typedef enum { QD_DRV_LW300, QD_DRV_LS, QD_DRV_SEL } qd_driver_t;

// The values 85/86, 87 and 88/89 carry, already negative.
typedef struct { int16_t top, left, width; } qd_setup_t;

// One sheet: Apple's paper and image area (from its driver), and the page a client sends for
// the IPP size at 300 dpi, which may differ by a few dots (A4).
typedef struct {
    const char *media;            // "na_letter_8.5x11in" ...
    uint16_t paper_w, paper_h;    // Apple's paper in dots at 300 dpi
    uint16_t page_w, page_h;      // the page a client sends at 300 dpi for the IPP size
    uint16_t img_w, img_h;        // Apple's image area, the rows the rip keeps; img_w a multiple of 8
    qd_setup_t set;               // the driver's own setup for the sheet (the LS); zero: the
                                  // 300's rule (qd_setup). A row is -set.width bytes, img_w
                                  // dots and white after.
    uint8_t  size;                // 83 9D's paper number
} qd_media_t;

typedef struct {
    const char *name;             // "Personal LaserWriter 300"
    uint8_t  engine;              // 83 3E's answer
    qd_driver_t driver;
    // A job's source unless it asks for the manual one: the 300's cassette, 83 4C, or the LS's
    // multipurpose tray, 83 52 (its cassette, 83 51, is an optional feeder; the tray feeds a
    // stack or a sheet by hand). src_manual: the 300's manual slot, 83 4F; 0 on the LS.
    uint8_t  src_main, src_manual;
    uint8_t  bit_cassette, bit_manual;   // 83 2A: 0x20, 0x40 (QD_DRV_LW300 only)
    uint8_t  density_cmd, density;       // 0xA7, 32; 0: none sent
    uint8_t  row_bits;            // the row packet's length field: 12, or 9 (qd_wire.h)
    uint32_t budget;              // bytes of row codes a page may take: 500000
    const uint8_t *screen8, *tone;       // apple_lw_screen8, apple_lw_tone
    const qd_media_t *media; uint8_t n_media;   // Letter first
    const char *icon;             // "lw4600"
} qd_spec_t;

const qd_spec_t      *qd_spec(qd_model_t m);                 // NULL for QD_NONE or out of range
// QD_PLW300 for 0x29, QD_PLWLS for 0x0D, QD_NONE for 0x23 (a Japanese model), else QD_SEL300.
qd_model_t            qd_model_from_engine(uint8_t engine);
// The printer an engine identity names, for a person: 0x0D the LS, 0x23 none (a Japanese
// model, NULL), 0x29 the 300, any other the Select 300.
const char           *qd_engine_name(uint8_t engine);
// The sheet a page of w dots across (the image width the rip keeps) and h rows is on: the
// sheet whose img_w is w and whose page_h is within 2 of h; else by w alone; else the first.
// NULL for a model with none.
const qd_media_t     *qd_media_for(qd_model_t m, uint32_t w, uint32_t h);
qd_setup_t            qd_setup(const qd_media_t *md);
// W-1146: a Select 300 on page memory takes its left margin and row width in dots: left
// (2550 - image w + 0x16A) / 2 (sent as 87, its low byte, and 8C, its high byte), the width
// the image's. The top is qd_setup's. Its driver caps the left at 0x1FF dots, which none of
// its three sheets reaches.
enum { QD_SEL_ENGINE_W = 2550, QD_SEL_LEFT = 0x16A };
qd_setup_t            qd_setup_pm(const qd_media_t *md);
size_t                qd_row_bytes(const qd_media_t *md);     // -qd_setup(md).width
const printer_caps_t *qd_caps(qd_model_t m);                  // NULL for QD_NONE
