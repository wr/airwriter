// SPDX-License-Identifier: GPL-3.0-or-later
//
// A StyleWriter's test page (W-1103, design "Status, panel, test page, settings"; the look
// is the ImageWriter's, docs/superpowers/specs/2026-10-20-test-page-design.md). A StyleWriter
// has no fonts, so the page is a raster drawn on the board: rows at 360 dpi, one Letter page,
// given to the same stage a job's rows go to (sw_page.h), so it takes the model's margins,
// bands and run coder as any page does.
//
// Top to bottom: the letterhead (the Chooser icon, "AirWriter" and "Test page" in Apple
// Garamond Light, sw_art.h), the board's details in a small 5 x 7 face (sw_font.h), the
// inks (seven solid swatches, the four inks and the three they make in pairs, then the still
// life and a ramp of each ink, with a color cartridge; the still life in gray, a gray scale
// and a black ramp without), the six screens side by side with the one in use ticked, a ruler
// 8 in long with a tick every 0.1 in, four squares an inch each way, and the footer.
//
// Built as it is sent, a row at a time: it holds a few halftoners, a picture's reader and the
// row's four planes, each part's only while its rows are made (about 173 KB asked over a gray
// page and 226 KB over a color one on the board, through bigmem.h: PSRAM), and never a page.
// The still life and the scene the panels show are not traced here: tools/rip/mkscene.c
// traces them once (tp_scene.c) and sw_scene.h stores them deflated, read a row at a time
// (sw_unpack.h, its window held only while a picture's rows are being made). Traced on the
// board, they kept the printer waiting partway down the page (W-1133).
//
// The layout is in dots from the printable area's top-left (the engine's margins are dropped
// by the stage), 2880 across and 3295 down at the most: the same on every model, which print
// 3774 rows at the fewest. Nothing is drawn where a model cannot print, and the stage is given
// the rows the margins cover as blank ones.
//
// W-1102: another family's page is the same page at its own dpi (sw_tp_geom_t): every size in
// dots at 360 dpi is scaled by dpi / 360 (the 8 in layout is 2400 dots at 300 dpi, the inch
// squares 300), the face is drawn 3 dots a pixel, the pictures are resampled from the stored
// scenes, the letterhead is the family's own art, and the page is gray. The Personal
// LaserWriter 300's is qd_testpage.h's.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ripjob.h"
#include "sw_model.h"
#include "halftone.h"

// A letterhead at a page's dpi (mkart.py writes them): "AirWriter" and "Test page" as bit rows
// (MSB first, (w + 7) / 8 bytes a row), and the printer's icon as indexes into rgb.
typedef struct {
    uint16_t title_w, title_h, sub_w, sub_h, icon_w, icon_h;
    const uint8_t *title, *sub, *icon;
    const uint8_t (*rgb)[3];
} sw_tp_art_t;

// The page for a family that is not a StyleWriter: its dpi, the rows the stage is given and
// where the image area sits on the sheet, and what is the family's own. The tone is none: the
// page is screened as a job's gray rows are for that family (the gain alone).
typedef struct {
    uint16_t    dpi;                            // 300
    uint16_t    sheet_w, sheet_h;               // each row's dots (the image area's width, at
                                                // least the layout's 8 in) and the sheet's rows
    uint16_t    margin_l, margin_t, margin_b;   // the image area on the sheet; the layout
                                                // starts margin_t rows down
    const char *model_name;                     // under Printer
    ht_algo_t   apple;                          // the family's Apple screen, for HT_APPLE
    const sw_tp_art_t *art;                     // the letterhead, at dpi
    const char *const (*captions)[4];           // the squares' two lines of captions
} sw_tp_geom_t;

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
    // W-1102: NULL for a StyleWriter's page (from `model`); another family's geometry, which
    // makes `model` and `colour_cart` unused.
    const sw_tp_geom_t *geom;
} sw_tp_info_t;

enum { SW_TP_DONE = 0, SW_TP_NOMEM = -1, SW_TP_STAGE = -2, SW_TP_STOPPED = 1 };

// One Letter page of rows to the stage: page_begin at the printable width and the sheet's
// 3960 rows (with a geometry, its sheet_w and sheet_h), every row (ink_row when the model
// takes a color cartridge and one is in, else row), then page_end. SW_TP_DONE; SW_TP_NOMEM
// for a bad model or no memory (nothing is left held, no page was ended); SW_TP_STAGE when the stage is missing a function the page
// needs or refused the page; SW_TP_STOPPED.
int sw_testpage(const sw_tp_info_t *i, const rip_stage_t *st);

// Where the parts of the page are, in dots from the printable area's top-left, for the tests
// and the host preview to find them by. The same code that draws the page fills it.
typedef struct { uint16_t x, y, w, h; } sw_tp_box_t;

#define SW_TP_SWATCHES 7               // the colour row's swatches

typedef struct {
    sw_tp_box_t letterhead;              // the icon, the title and "Test page"
    sw_tp_box_t details;                 // the board's details, in the face
    sw_tp_box_t swatch[SW_TP_SWATCHES];  // cyan, magenta, yellow, black, then red (magenta over
                                         // yellow), green (cyan over yellow) and blue (cyan over
                                         // magenta), solid (color cartridge)
    sw_tp_box_t photo;                   // the still life through the screen in use
    sw_tp_box_t ink_ramp[4];             // 0 to 100 % of cyan, magenta, yellow, black (black alone: [3])
    sw_tp_box_t gray_step[11];           // 0 to 100 % in tens (gray page)
    sw_tp_box_t screen_names;            // their names, over the panels
    sw_tp_box_t panel[6];                // the still life through each screen
    sw_tp_box_t panel_ramp[6];           // and a ramp
    ht_algo_t   panel_screen[6];         // which screen (the family's Apple screen for Apple's)
    int         ticked;                  // the panel ticked: the one info->screen names, or -1
    sw_tp_box_t ruler;                   // its edge and ticks (numbers below it are not in it)
    sw_tp_box_t square[4];               // an inch each way: 360 x 360 dots at 360 dpi
    sw_tp_box_t footer;                  // the last line
    uint16_t    first_ink, last_ink;     // the first and last rows that carry ink
    int         colour;                  // the page is in four inks
} sw_tp_layout_t;

void sw_testpage_layout(const sw_tp_info_t *i, sw_tp_layout_t *out);

// The page's size in dots, the rows given to the stage and the columns the layout needs.
#define SW_TP_SHEET_H 3960u
#define SW_TP_SHEET_W 3060u
#define SW_TP_W       2880u
