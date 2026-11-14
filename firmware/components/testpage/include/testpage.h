// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page every printer prints (W-1140; design docs/superpowers/specs/2026-10-20-test-
// page-design.md, its amendment block). One page (Letter; a LaserWriter's A4 or Legal) drawn on
// the board as rows of dots, the same page on every printer: its icon, its name and whether it
// prints in color are what change.
// The rows go to the stage a phone's page goes to (ripjob.h rip_stage_t): the ImageWriter's
// strips (iw_stage.h), a StyleWriter's bands (sw_page.h), the Personal LaserWriter 300's packets
// (qd_page.h), a LaserWriter's PostScript (ps_page.h). What is a family's own is in a tp_geom_t:
// its dots across and down, its sheet, how its jobs make a screen, the squares' captions
// (tp_iw.h, sw_testpage.h, qd_testpage.h, lw_testpage.h).
//
// Top to bottom: the letterhead (the printer's icon, tp_icons.h, beside "AirWriter" and "Test
// page" in Apple Garamond Light, tp_words.h), the board's details in a small 5 x 7 face
// (tp_face.h), the type tester (a sans and a serif at five sizes, tp_type.h, averaged down to the
// page's dots as a phone's page is and screened with the screen in use), the inks (seven solid
// swatches and a ramp of each ink in color; eleven gray steps and a black ramp in gray), the
// screens side by side with the one in use ticked, a ruler 8 in long with a tick every 0.1 in,
// four squares an inch each way, and the footer.
//
// Built as it is sent, a row at a time: each part holds its halftoners and readers only while
// its own rows are made (through bigmem.h: PSRAM on the board), never a page. The still life the
// screens show is traced on the host once (tools/rip/mkscene.c, tp_pictures.h, W-1133).
//
// The layout is 8 in across and 9.5 in down at the most, the ImageWriter's printable sheet, the
// least of any printer's; its sizes are dots at 360 dpi, scaled across and down apart to the
// family's dots.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ripjob.h"
#include "halftone.h"

#define TP_W_360    2880u           // the layout across, at 360 dpi: 8 in
#define TP_H_360    3420u           // and the most it may take down: 9.5 in

// What is a family's own. The tests hold every family's to the layout.
typedef struct {
    uint16_t    dpi_x, dpi_y;       // the page's dots: 144 x 72 on an ImageWriter
    uint16_t    sheet_w, sheet_h;   // the rows the stage is given: sheet_w dots each, sheet_h of them
    uint16_t    left, top;          // where the layout starts on the sheet: dots in from each row's
                                    // left (left + the layout's 8 in within sheet_w), and rows down
    uint16_t    print_w, print_h;   // the printable area, in dots, for the Page line
    const char *paper;              // the sheet's name for the Page line; NULL: "Letter"
    uint16_t    margin_l, margin_t, margin_b;   // its margins on the sheet, for the Margins line
    char        model[96];          // the Printer line: "ImageWriter II, color ribbon"
    const char *icon;               // the printer's icon by its AirPrint name (pr_icon); NULL or
                                    // unknown: the ImageWriter's
    int         colour;             // the page is in four inks (the stage takes ink_row)
    const char *gray_note;          // a gray page on a printer that prints in color: why. NULL: none
    int         apple;              // the family has its own Apple screen: six panels, else five
    ht_algo_t   apple_algo;         // what the record's "apple" is: HT_APPLE, HT_APPLE_LW
    // The family's tone curve for a screen (sw_tone), put on every gray before its screen as its
    // jobs do. NULL: none.
    const uint8_t *(*tone)(const void *ctx, ht_algo_t algo);
    const void *tone_ctx;
    int         ink_place;          // each screen placed for its ink as the family's jobs place it
                                    // (ink.h ink_screen_phase; Apple's a row on)
    const char *const (*captions)[4];   // the squares' two lines; NULL: the ImageWriter's
} tp_geom_t;

typedef struct {
    const char *printer;            // its name: "Den StyleWriter"
    const char *board;              // "v0.4.0 at 10.0.1.115", after the AirWriter label
    const char *settings;           // where the settings page is: "http://airwriter-2434.local",
                                    // or on the setup network "http://192.168.4.1, join AirWriter-2434"
    ht_algo_t   screen;             // the screen pages print with, ticked (HT_APPLE: the family's own)
    int         gain;               // the record's dot gain, halftone.h ht_set_gain
    // Asked every 32 rows: non-zero stops the page where it is (no page end follows) and tp_page
    // returns TP_STOPPED. NULL never stops.
    int       (*stop)(void *ctx);
    void       *stop_ctx;
    const tp_geom_t *geom;
} tp_info_t;

enum { TP_DONE = 0, TP_NOMEM = -1, TP_STAGE = -2, TP_STOPPED = 1 };

// One page of rows to the stage, on Letter or the sheet the geometry names: page_begin at
// sheet_w by sheet_h, every row (ink_row on a
// color page, else row), then page_end. TP_DONE; TP_NOMEM for a geometry the page cannot be drawn
// at or no memory (nothing is left held, no page was ended); TP_STAGE when the stage is missing a
// function the page needs or refused the page; TP_STOPPED.
int tp_page(const tp_info_t *i, const rip_stage_t *st);

// Where the parts of the page are, in the page's dots from the layout's top-left (sheet row
// `top`), for the tests and the host preview to find them by. The same code that draws the page
// fills it.
typedef struct { uint16_t x, y, w, h; } tp_box_t;

#define TP_SWATCHES 7               // the color row's swatches
#define TP_PANELS   6               // the screens, at the most

typedef struct {
    tp_box_t    letterhead;              // the icon, the title and "Test page"
    tp_box_t    icon;                    // the icon itself
    tp_box_t    details;                 // the board's details, in the face
    tp_box_t    type;                    // the type tester's samples
    tp_box_t    type_line[5];            // each size's line, both columns, baseline at its bottom
    tp_box_t    swatch[TP_SWATCHES];     // cyan, magenta, yellow, black, then red (magenta over
                                         // yellow), green (cyan over yellow) and blue (cyan over
                                         // magenta), solid (a color page)
    tp_box_t    ink_ramp[4];             // 0 to 100 % of cyan, magenta, yellow, black (gray: [3])
    tp_box_t    gray_step[11];           // 0 to 100 % in tens (a gray page)
    tp_box_t    screen_names;            // their names, over the panels
    int         panels;                  // five, or six with the family's Apple screen
    tp_box_t    panel[TP_PANELS];        // the still life through each screen
    tp_box_t    panel_ramp[TP_PANELS];   // and a ramp
    ht_algo_t   panel_screen[TP_PANELS]; // which screen (the family's Apple screen for Apple's)
    int         ticked;                  // the panel ticked: the one info->screen names, or -1
    tp_box_t    ruler;                   // its edge and ticks (numbers below it are not in it)
    tp_box_t    square[4];               // an inch each way
    tp_box_t    footer;                  // the last line
    uint16_t    first_ink, last_ink;     // the first and last rows that carry ink
    int         colour;                  // the page is in four inks
} tp_layout_t;

// The layout tp_page would draw; all zeros for a geometry it cannot.
void tp_layout(const tp_info_t *i, tp_layout_t *out);
