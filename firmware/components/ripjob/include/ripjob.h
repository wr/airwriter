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

// The one pass v1 has: 144 dpi across, 72 down. SPEC.md 3.8.
#define RIPJOB_DPI_X 144
#define RIPJOB_DPI_Y  72

typedef struct {
    // Printer stream out. Required.
    void      (*out)(void *ctx, const uint8_t *buf, size_t n);
    void       *out_ctx;

    ht_algo_t   dither;
    // Dot gain at a 50% tint, percent (halftone.h ht_set_gain). 0 prints
    // the bits as they are.
    int         gain;

    // The colour ribbon is in (W-1064): RGB pages print in four inks. With
    // 0 they print as gray, as a black ribbon would anyway. Gray pages print
    // the same either way.
    int         colour;

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
} ripjob_cfg_t;

typedef struct {
    int      pages;
    uint32_t dst_w, dst_h;      // of the last page started
    uint32_t crop_per_side;     // source pixels dropped per side, last page
} ripjob_stats_t;

// Runs every page in an already-opened URF stream. Returns 0, or a negative
// value on a truncated stream, an unsupported page or an allocation failure.
// `st` may be NULL.
int ripjob_run(urf_t *u, const ripjob_cfg_t *cfg, ripjob_stats_t *st);
