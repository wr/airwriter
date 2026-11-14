// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the page stage for a QuickDraw LaserWriter, the rip's last step, as sw_page.h is for a
// StyleWriter. It takes the page's 300 dpi rows, keeps the sheet's image area (the rows from
// (page_h - img_h) / 2 down, img_w dots across, cut or padded with white), run-codes each row on
// its own (sw_code_row with no row before) and hands the codes to a sink as they are made.
//
// The printer holds about half a megabyte of codes and a page that runs out mid-load is lost, so
// the stage counts what it sends against a budget. While the record's screen is not Apple's, it
// also screens each row's gray in Apple's 300 screen (HT_APPLE_LW, from the gray_row hook) and
// keeps those codes in a buffer of the caller's. A page that passes the budget stops being sent;
// at its end the sink is opened again for the same page (which drops the rows of it the printer
// holds, once the page printing before it is done) and the kept page goes instead. A page that passes it in both fails: the sink is told so, and the job goes on.
// A record set to Apple's screen keeps nothing: ripjob screens its rows in Apple's screen.
// Pure C; tools/rip builds it on the host.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "ripjob.h"
#include "halftone.h"
#include "qd_model.h"

typedef struct {
    // The page's setup; page is its index in the job, as note's. Opened a second time before
    // its print, the rows sent since the first are dropped (qd_session_open). Non-zero aborts
    // the job.
    int  (*open)(void *ctx, int page, const qd_media_t *md);
    // One row's codes, top to bottom. Non-zero aborts the job.
    int  (*row)(void *ctx, const uint8_t *codes, size_t n);
    // Every row is in: check and print, once the page before it is done (W-1144: it returns as
    // the page starts to print). Non-zero aborts the job.
    int  (*print)(void *ctx);
    // The page is settled: apple 1 if it went in Apple's screen, bytes its codes, too_big 1 if
    // it fit neither screen (nothing was printed). May be NULL.
    void (*note)(void *ctx, int page, int apple, uint32_t bytes, int too_big);
    void  *ctx;
} qd_sink_t;

typedef struct {
    qd_model_t model;
    ht_algo_t  screen;      // the record's; HT_APPLE is Apple's 300 screen alone
    int        gain;        // the record's dot gain (halftone.h), for the kept page's screen
    uint8_t   *keep;        // where the Apple-screen page is kept, or NULL: the page then goes in
    size_t     keep_cap;    // Apple's screen alone. Each row is kept as [u16 LE n][codes].
    uint32_t   budget;      // bytes of codes a page may take; 0: the model's (500000)
    qd_sink_t  sink;
} qd_page_cfg_t;

typedef struct qd_page qd_page_t;

// The keep buffer a job gives the stage: a page at the budget kept whole, with each row's
// two-byte head (Legal's 3,750 rows), rounded up.
enum { QD_KEEP_CAP = 520 * 1024 };

// Allocates the stage and a row's buffers, once (bigmem.h); the halftoner for the kept page is
// made per page. NULL for QD_NONE, a sink without open, row or print, or no memory.
qd_page_t *qd_page_new(const qd_page_cfg_t *cfg);
// What ripjob screens `row` with: cfg->screen, or HT_APPLE_LW when that is HT_APPLE (or
// HT_APPLE_LW) or there is no keep buffer.
ht_algo_t  qd_page_rip_screen(const qd_page_cfg_t *cfg);
// The stage to hand ripjob_cfg_t: page_begin, row, page_end, and gray_row only while a page is
// kept. ink_row is NULL: the printer is black only.
void       qd_page_stage(qd_page_t *p, rip_stage_t *st);
// Pages so far that fit neither screen.
int        qd_page_failed(const qd_page_t *p);
void       qd_page_free(qd_page_t *p);
