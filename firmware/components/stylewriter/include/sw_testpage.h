// SPDX-License-Identifier: GPL-3.0-or-later
//
// A StyleWriter's test page (W-1103, design "Status, panel, test page, settings"; the look
// is the ImageWriter's, docs/superpowers/specs/2026-10-20-test-page-design.md). A StyleWriter
// has no fonts, so the page is a raster drawn on the board: rows at 360 dpi, one Letter page,
// given to the same stage a job's rows go to (sw_page.h), so it takes the model's margins,
// bands and run coder as any page does.
//
// Top to bottom: the letterhead (the Chooser icon, "AirWriter" and "Test page" in Apple
// Garamond Light, sw_art.h), the board's details in a small face of our own (sw_font.h), the
// inks (four solid swatches, the still life and a ramp of each ink with a color cartridge; the
// still life in gray, a gray scale and a black ramp without), the six screens side by side
// with the one in use ticked, a ruler 8 in long with a tick every 0.1 in, four squares an
// inch each way, and the footer.
//
// Built as it is sent, a row at a time: it holds a few halftoners and the row's four planes
// (88 KB gray, 141 KB color at its largest, through bigmem.h: PSRAM on the board) and never a
// page. The still life is ray traced as its rows are made, at half the page's resolution and
// enlarged, so a page costs the board some seconds of its own between the printer's blocks;
// the host draws one in under a fifth of a second.
//
// The layout is in dots from the printable area's top-left (the engine's margins are dropped
// by the stage), 2880 across and 3295 down at the most: the same on every model, which print
// 3774 rows at the fewest. Nothing is drawn where a model cannot print, and the stage is given
// the rows the margins cover as blank ones.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ripjob.h"
#include "sw_model.h"
#include "halftone.h"

typedef struct {
    const char *printer;       // its name: "Den StyleWriter"
    const char *board;         // "AirWriter v0.3.0 at 10.0.1.115"
    const char *host;          // "airwriter-2434.local"
    sw_model_t  model;         // the geometry, and the name under Printer
    int         colour_cart;   // a color cartridge is in: ink rows, else gray rows
    int         gain;          // the record's dot gain, halftone.h ht_set_gain
    // The page's own: the screen to tick among the six, and a hook to stop on.
    ht_algo_t   screen;        // the screen pages print with, ticked among the six
    // Asked every 32 rows: non-zero stops the page where it is (no page end follows) and
    // sw_testpage returns SW_TP_STOPPED. NULL never stops.
    int       (*stop)(void *ctx);
    void       *stop_ctx;
} sw_tp_info_t;

enum { SW_TP_DONE = 0, SW_TP_NOMEM = -1, SW_TP_STAGE = -2, SW_TP_STOPPED = 1 };

// One Letter page of rows to the stage: page_begin at the printable width and the sheet's
// 3960 rows, every row (ink_row when the model takes a color cartridge and one is in, else
// row), then page_end. SW_TP_DONE; SW_TP_NOMEM for a bad model or no memory (nothing is
// left held, no page was ended); SW_TP_STAGE when the stage is missing a function the page
// needs or refused the page; SW_TP_STOPPED.
int sw_testpage(const sw_tp_info_t *i, const rip_stage_t *st);

// Where the parts of the page are, in dots from the printable area's top-left, for the tests
// and the host preview to find them by. The same code that draws the page fills it.
typedef struct { uint16_t x, y, w, h; } sw_tp_box_t;

typedef struct {
    sw_tp_box_t letterhead;              // the icon, the title and "Test page"
    sw_tp_box_t details;                 // the board's details, in the face
    sw_tp_box_t swatch[4];               // cyan, magenta, yellow and black, solid (color cartridge)
    sw_tp_box_t photo;                   // the still life through the screen in use
    sw_tp_box_t ink_ramp[4];             // 0 to 100 % of cyan, magenta, yellow, black (black alone: [3])
    sw_tp_box_t gray_step[11];           // 0 to 100 % in tens (gray page)
    sw_tp_box_t screen_names;            // their names, over the panels
    sw_tp_box_t panel[6];                // the still life through each screen
    sw_tp_box_t panel_ramp[6];           // and a ramp
    ht_algo_t   panel_screen[6];         // which screen
    sw_tp_box_t ruler;                   // its edge and ticks (numbers below it are not in it)
    sw_tp_box_t square[4];               // 360 x 360 dots each
    sw_tp_box_t footer;                  // the last line
    uint16_t    first_ink, last_ink;     // the first and last rows that carry ink
    int         colour;                  // the page is in four inks
} sw_tp_layout_t;

void sw_testpage_layout(const sw_tp_info_t *i, sw_tp_layout_t *out);

// The page's size in dots, the rows given to the stage and the columns the layout needs.
#define SW_TP_SHEET_H 3960u
#define SW_TP_SHEET_W 3060u
#define SW_TP_W       2880u
