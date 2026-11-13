// SPDX-License-Identifier: GPL-3.0-or-later
//
// One print job, from a URF stream to ImageWriter bytes.
//
// This is the whole page loop: crop, resample, halftone, transpose, emit. It
// lives in a component rather than in tools/rip so the firmware and the host
// harness run the same code -- which is what makes the byte-for-byte fixture
// regression in tools/rip meaningful for the device.
//
// Nothing here allocates a page. Per page it holds one halftone error ring
// (3 rows), one resampler row, one 1bpp row and one decoded URF line, so a
// Letter page costs about 10KB regardless of length. A colour page (W-1064)
// holds the same per ink, four of them, and its strips: about 60KB.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "urf_decode.h"
#include "halftone.h"
#include "imagewriter.h"
#include "ink.h"
#include "printer_caps.h"

// The ImageWriter's one pass: 144 dpi across, 72 down. SPEC.md 3.8. The same
// as printer_caps_imagewriter's rip_dpi_x/y, which is what ripjob uses.
#define RIPJOB_DPI_X 144
#define RIPJOB_DPI_Y  72

// An output stage other than the ImageWriter's strips (W-1082): a LaserWriter's
// PostScript bands (ps_page.h), a StyleWriter's bands (sw_page.h, W-1103). It
// takes the page's 1bpp rows in order, MSB the leftmost pixel and a set bit ink,
// as the halftoner makes them.
typedef struct {
    // A page starts; w x h rows follow. Non-zero aborts the job.
    int  (*page_begin)(void *ctx, int index, uint32_t w, uint32_t h);
    void (*row)(void *ctx, const uint8_t *bits);
    // The page's last row has been given. Non-zero aborts the job.
    int  (*page_end)(void *ctx);
    void  *ctx;
    // Set: an RGB page, when the job is in colour, comes as four rows a pixel
    // row, one per ink indexed by iw_ink_t, screened as the colour ribbon's are
    // (ink.h), and `row` is not called for it. NULL: RGB pages are gray, as for a
    // LaserWriter. Gets ctx.
    void (*ink_row)(void *ctx, const uint8_t *const bits[IW_INKS]);
    // Set (W-1102): each gray page row, resampled and before its screen (0 black, w bytes), ahead
    // of `row` for the same row, for a stage that screens a page a second way. A colour page's
    // rows come through it as gray. Gets ctx.
    void (*gray_row)(void *ctx, const uint8_t *gray, uint32_t w);
} rip_stage_t;

typedef struct {
    // Printer stream out. Required for the ImageWriter; unused with a stage.
    void      (*out)(void *ctx, const uint8_t *buf, size_t n);
    void       *out_ctx;

    // The printer's geometry: resolution, widest row, the ImageWriter's
    // quarter-inch crop. NULL is printer_caps_imagewriter.
    const printer_caps_t *caps;
    // Where rows go instead of the ImageWriter's strips. With a stage, RGB
    // pages print as gray (colour is ignored) unless the stage takes ink rows,
    // and top_margin is ignored: both are the ImageWriter's. A page wider than
    // caps->max_dots is clipped on the right when caps->crop_margin is 0. With
    // caps->n_media, each page is cropped by the sheet its width names instead:
    // rip_w dots from rip_left, or as far as the page goes (printer_caps.h).
    const rip_stage_t *stage;

    // The screen. Apple's (HT_APPLE) is placed as the Color StyleWriter drivers place
    // it: a gray page on the black ink's placement, and every page, gray or colour,
    // starting on the tile's second row (halftone.h, ink.h). The others start
    // where they always have.
    ht_algo_t   dither;
    // Dot gain at a 50% tint, percent (halftone.h ht_set_gain). 0 prints
    // the bits as they are.
    int         gain;

    // The colour ribbon is in (W-1064): RGB pages print in four inks. With
    // 0 they print as gray, as a black ribbon would anyway. Gray pages print
    // the same either way. With a stage, only a stage with ink_row has it.
    int         colour;

    // The stage printer's tone curve (apple_tables.h, sw_tone), put on every gray
    // before its screen, and on R, G and B before a colour pixel is split into
    // inks. NULL is none. Kept by pointer, so it has to outlive the job.
    const uint8_t *tone;

    // The top margin setting, in 1/144 in: positive adds that much blank
    // paper ahead of each page, negative drops the page's first rows, which
    // past its own blank margin crops what is on it. In whole printer rows
    // (1/72 in), rounding toward zero. Only the printer stream moves: every
    // row is still screened and shown to `row`, so the rows kept print the
    // bits they would with no margin.
    int         top_margin;

    // Called as each page starts, with the geometry every later row will
    // have, before any row arrives. Return non-zero to abort the job.
    int       (*page)(void *ctx, int index, uint32_t dst_w, uint32_t dst_h);
    void       *page_ctx;
    // Called as each page's last byte has gone to `out` (the ImageWriter's form feed), or
    // the stage's page_end has taken it, with the pages whole so far, 1 the first (W-1141).
    // Never for a page that failed. Gets page_ctx.
    void      (*page_done)(void *ctx, int pages);

    // Called once per halftoned destination row, in order, before it reaches
    // the printer stage. The host tool builds its preview from this; the
    // firmware leaves it NULL.
    void      (*row)(void *ctx, const uint8_t *bits, uint32_t w, uint32_t y);
    void       *row_ctx;
    // The same for a colour page: one 1bpp row per ink, indexed by iw_ink_t.
    // Gets row_ctx.
    void      (*ink_row)(void *ctx, const uint8_t *const bits[IW_INKS], uint32_t w, uint32_t y);

    // Human-readable diagnostics, already formatted. NULL stays silent.
    void      (*log)(void *ctx, const char *msg);
    void       *log_ctx;

    // W-1143: a microsecond clock (it may wrap). Set, each page ends with a log line of where
    // its time went (ripjob_run). NULL: no line.
    uint32_t  (*clock_us)(void);
} ripjob_cfg_t;

typedef struct {
    int      pages;
    uint32_t dst_w, dst_h;      // of the last page started
    uint32_t crop_per_side;     // source pixels dropped per side, last page
} ripjob_stats_t;

// Runs every page in an already-opened URF stream. Returns 0, or a negative
// value on a truncated stream, an unsupported page or an allocation failure.
// `st` may be NULL.
//
// With cfg->clock_us, each page that ends logs "page N: T ms: read R, scale S, screen H,
// second screen K, out O, end E", in ms: R reading its rows (URF decoding, and waiting for
// the upload), S the gray conversion and resampling, H the job's screen (or the four inks'),
// K a stage's gray_row (the Personal LaserWriter 300's Apple-screen page, W-1102), O the
// rows going on (the row callback, the stage's row or ink_row, or the ImageWriter's strips),
// E the page's end (a stage's page_end: the 300's print and wait for page done).
int ripjob_run(urf_t *u, const ripjob_cfg_t *cfg, ripjob_stats_t *st);
