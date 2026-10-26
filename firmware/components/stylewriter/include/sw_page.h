// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter's page stage (W-1103, design "The page"): the rip's last step
// for a StyleWriter, in the place the LaserWriter's PostScript stage has. It takes
// the page's 360 dpi rows (one K plane, or four planes when the job is in colour),
// drops the engine's margins, collects the rows into bands of the model's height,
// run-codes each non-blank band (sw_code.h) and writes it out as frames. The
// session (sw_session.h) turns the frames into the conversation with the printer:
// the page commands, then each rect and block as the printer's buffer has room,
// then the page's end. Pure C; tools/rip builds it on the host.
//
// A frame is [type][u16 LE length][payload], written whole in one call to `out`:
//   SWF_PAGE      1 byte: 1 if the page prints in colour (c rects, four planes a row),
//                 0 if black (R rects, K only). Before the page's first rect, and
//                 always, even for a blank page.
//   SWF_RECT      the wire bytes of a band's rectangle: 'R' (black) or 'c' (four
//                 inks), then left, top, right, bottom as u16 LE. Left and right
//                 include the model's offset (16 on the II, 1200 and 1500); top and
//                 bottom count from the printable area's first row.
//   SWF_BLOCK     the coded data of one `G` block: whole rows (every plane of a row,
//                 C, M, Y, K), never more than sw_block_cap bytes. The session adds
//                 the `G`, the length and the 00. The rows after a block's first
//                 are XORed with the row before, as it was in the band, so the
//                 delta carries over from one block to the next in a rect.
//   SWF_PAGE_END  empty. The session sends 0C.
// A band with no ink in it has no rect or block: the next rect's top moves on.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ripjob.h"
#include "sw_model.h"

enum { SWF_PAGE = 1, SWF_RECT = 2, SWF_BLOCK = 3, SWF_PAGE_END = 4 };

typedef struct {
    sw_model_t model;
    int        colour_cart;                 // 0: four-plane rows print as K only
    void     (*out)(void *ctx, const uint8_t *frame, size_t n);   // one whole frame per call
    void      *ctx;
} sw_page_cfg_t;

typedef struct sw_page sw_page_t;

// Allocates the band (band_rows x planes x row bytes: PSRAM on the board, through
// bigmem.h) and the block. NULL for SW_NONE, a missing `out`, or no memory.
// colour_cart is ignored for a model that takes no colour cartridge.
sw_page_t *sw_page_new(const sw_page_cfg_t *cfg);

// The stage to hand ripjob_cfg_t: page_begin, row, page_end and ink_row on `p`.
// The pages it is fed are the engine's printable width across (ripjob crops to the
// sheet's rip_left and rip_w, printer_caps.h) and the whole sheet's height down; the
// engine's top and bottom margins are dropped here. A row wider than the widest
// printable width of any sheet the model offers is cut to it.
void       sw_page_stage(sw_page_t *p, rip_stage_t *st);

void       sw_page_free(sw_page_t *p);
