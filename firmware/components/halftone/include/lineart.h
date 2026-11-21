// SPDX-License-Identifier: GPL-3.0-or-later
//
// Crisp text and thin lines (W-1174), for a printer whose toner spreads: gray rows in, the
// same rows out ten rows later, with text and thin lines on paper decided ahead of the screen.
//
// A page's text reaches the board as gray with soft edges (the sender's anti-aliasing), and a
// screen would print those edges, and gray text itself, as scattered dots. Here, a pixel on a
// thin dark feature that sits on paper or near-paper (a light tint) becomes black or paper:
//   - dark (191 or darker): black where it is darker than halfway between paper and the
//     darkest pixel within 2 dots, else paper, so text keeps its weight and loses its fringe.
//     Gray text prints black.
//   - light (192 to `light`, 231 unless set) with white paper (250 or lighter) within 4 dots on
//     both sides, up and down or left and right: a thin light line, printed one dot wide and
//     unbroken: black where it is the darkest across the line (the first of its darkest run)
//     and the line goes on along it (a light dot beside it along the line, straight or through
//     a corner, on either side, so a lone pale dot is no line and a slanted line is), else
//     paper. A photo's pale grain, or a line on a tint, has no white
//     beside it and is the screen's; next to a dark one it is that one's soft edge, paper. A
//     printer whose rows average a hairline nearly away sets `light` higher (W-1178: the
//     ImageWriter's 1/72 in rows, 247).
//   - 232 to 252 next to a dark one, or next to a light one with white paper beside it: the
//     soft edge of text or a line, paper.
// "Thin" and "on paper": the 7 x 7 closing of {gray >= 236} holds the pixel, so a feature
// narrower than 7 dots with paper or a light tint around it. A wider shape's edge (a pixel with
// paper or a tint within a dot) is crisp the same way where the shape is dark (191 or darker
// within a dot), so a QR code's squares and a bold headline keep sharp edges; their insides,
// near-black, the curve after this prints solid. Photos and gray areas pass through.
// Nor is anything touched within 6 dots of the core of a gray area (a 9 x 9 square all 49 to
// 200: so 2 dots past the edge of an area at least 9 wide), so a filled gray icon is screened
// whole, the thin parts next to it too.
//
// Rows beyond the page's edges count as paper. The work is bit planes of the row, 32 dots a
// word, so a row costs a few thousand word operations, and pixel by pixel only where there
// is ink near paper; a row with nothing but paper within LA_DELAY rows costs a few memsets.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define LA_DELAY 10                 // rows: a row comes out when the row 10 below it goes in

typedef struct {
    uint32_t w, words;              // dots, and 32-bit words, a row
    int32_t  r;                     // rows pushed, page rows and the flush's
    int32_t  h;                     // the page's rows once la_flush has begun, else -1
    int32_t  paper;                 // rows in a row that were all paper, this one the last
    int      light;                 // the lightest a thin light line may be: 231 (la_init)
    uint8_t  gset[16], zset[16];    // a gray-area ring row has a bit set, by its slot (lineart.c)
    uint8_t *gray;                  // the rows of gray held: LA_DELAY + 5
    uint32_t *plane;                // the bit planes, as many rows of each as are read back
    uint8_t *out;                   // the row handed back
} la_t;

// One for rows of w dots; nothing until la_page. -1 with no memory, nothing held: PSRAM only on
// the board (bigmem.h big_malloc_only), since a page prints without it.
// It asks for la_bytes(w) in two blocks: about 59 KB for a 300 dpi Letter row.
int  la_init(la_t *la, uint32_t w);
size_t la_bytes(uint32_t w);
void la_free(la_t *la);
// A page starts: everything above it is paper.
void la_page(la_t *la);
// A page row in (w bytes, 0 black). Out: the page row LA_DELAY above it, decided, or NULL
// while the first LA_DELAY rows go in. The pointer is good until the next call.
const uint8_t *la_push(la_t *la, const uint8_t *row);
// After the page's last row: the rows still held, one a call, then NULL.
const uint8_t *la_flush(la_t *la);
