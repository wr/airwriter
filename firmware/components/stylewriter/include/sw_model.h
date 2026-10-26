// SPDX-License-Identifier: GPL-3.0-or-later
//
// StyleWriters (W-1103, design docs/superpowers/specs/2026-10-25-stylewriters-
// design.md, "What the printer and the board allow"): the seven models, what
// each prints and how, and readers for the one-byte answers the printer gives
// to `?`, `p`, `H` and `B`. Pure C; tools/rip builds it on the host.
//
// The numbers are TailTalk's reading of Apple's drivers (family.rs, the
// drivers' ENGC resources) and lpstyl's; only the Color StyleWriter 2200 has
// printed from TailTalk on a real printer.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "printer_caps.h"
#include "halftone.h"

typedef enum { SW_NONE, SW_ORIGINAL, SW_II, SW_1200, SW_C1500, SW_C2200, SW_C2400, SW_C2500, SW_MODELS } sw_model_t;

typedef struct {
    const char *name;          // "StyleWriter II", "Color StyleWriter 2500"
    const char *make_model;    // "Apple StyleWriter II"
    uint16_t dots_across;      // 2880 / 2919: the widest row the engine prints
    uint16_t margin_l, margin_r, margin_t, margin_b;  // dots at 360 dpi from the sheet's edges
    uint16_t band_rows;        // 800 / 400
    uint16_t rect_dx;          // 0 or 16: added to a band rect's x values
    uint8_t  colour;           // takes a colour cartridge
    uint8_t  delta;            // rows XOR the row before (0 on the original)
    uint8_t  cleans;           // has `P` (0 on the original)
} sw_spec_t;

// NULL for SW_NONE and anything out of range.
const sw_spec_t *sw_spec(sw_model_t m);

// The `?` reply (to its CR, or the end of what came). SW II, SW3 the 1200,
// IJ10 the original. "CS" names a colour model but not which: SW_NONE with
// *is_cs set, and `p` (sw_model_from_p) says. Anything else is SW_NONE with
// *is_cs 0. is_cs may be NULL.
sw_model_t sw_model_from_id(const uint8_t *r, size_t n, int *is_cs);
// `p`'s byte, asked of a CS only: 01 the 2400, 02 the 2200, 04 the 1500, 05 the
// 2500, and any other the 2200.
sw_model_t sw_model_from_p(uint8_t p);

// `H`'s byte after `D`: 1 a colour cartridge, 0 black, -1 unreadable. A model
// that takes no colour cartridge reads 0; SW_NONE -1.
int sw_cart_colour(sw_model_t m, uint8_t h);

// `B`: the printer's free buffer is B x 256 - 2048 bytes (the original's B x 64),
// never below 0. sw_b_empty: B reads as an empty buffer on that model
// (`colour_cart` matters only to the 1500).
int32_t sw_free_bytes(sw_model_t m, uint8_t b);
int     sw_b_empty(sw_model_t m, uint8_t b, int colour_cart);

// The data bytes one `G` block may carry for a page of `planes` inks (1, or 4
// for colour): the model's cap, or the least an empty buffer can take less the
// block's 4 bytes of framing, whichever is less, so a block always fits.
uint32_t sw_block_cap(sw_model_t m, int planes);

// The widest row to print on paper `paper_w` dots wide: min(dots_across,
// paper_w less both side margins); 0 for SW_NONE or a paper too narrow.
uint32_t sw_printable_w(sw_model_t m, uint32_t paper_w);

// What AirPrint is told about the model, and how the rip lays a page out for it
// (printer_caps.h): 360 x 360 dpi, URF RS360, Letter, A4 and Legal with the
// engine's margins, each sheet's kept width from the printable area's left edge.
// One static table per model; NULL for SW_NONE and anything out of range.
const printer_caps_t *sw_caps(sw_model_t m);

// Apple's tone curve for a job (apple_tables.h): the Color StyleWriter 2200, 2400
// and 2500 take the 2200 driver's, the other four the 1500 driver's; GAMA 1 goes
// with the ordered and mask screens (Apple's, Bayer, blue noise, threshold) and
// GAMA 2 with error diffusion (Atkinson, Floyd-Steinberg). NULL for SW_NONE. The
// table is static; ripjob_cfg_t.tone keeps it by pointer.
const uint8_t *sw_tone(sw_model_t m, ht_algo_t algo);
