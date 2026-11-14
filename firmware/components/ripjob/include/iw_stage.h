// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ImageWriter's strips as a rip stage (ripjob.h rip_stage_t, W-1140): rows in, printer bytes
// out, through the same strip writers a phone's page goes through (imagewriter.h iw_*, and iwc_*
// for the color ribbon), with the top margin setting put on the page as ripjob puts it on a job's:
// a positive margin feeds that much blank paper ahead of the page, a negative one drops the page's
// first rows. The test page (testpage.h) prints through it.
#pragma once
#include <stdint.h>
#include "ripjob.h"
#include "imagewriter.h"

typedef struct {
    iw_out_fn out;
    void     *ctx;
    int       colour;              // rows come four inks at a time (ink_row), for the color ribbon
    int       top_margin;          // 1/144 in, as ripjob_cfg_t's
    uint32_t  skip;                // the page's rows still to drop
    iw_t     *iw;                  // the page's writer, held from page_begin to page_end
    iwc_t    *iwc;
} iw_stage_t;

void iw_stage_init(iw_stage_t *s, iw_out_fn out, void *ctx, int colour, int top_margin);
// The stage's functions over s: page_begin and page_end, and row (a gray page) or ink_row (colour).
void iw_stage(iw_stage_t *s, rip_stage_t *st);
// Frees what a page cut short left held. Takes a stage that holds nothing.
void iw_stage_free(iw_stage_t *s);
