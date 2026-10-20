// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page (docs/superpowers/specs/2026-10-20-test-page-design.md): a
// full Letter page in the ImageWriter II's own fonts and in bit images
// through the firmware's halftone, with the board's details at the top.
//
// Built as it is sent, line by line and strip by strip, so it never holds a
// page: the heaviest strip band holds four halftoners 432 dots wide, one more
// for its ramps and the strip buffers, all from the heap and freed after it.
#pragma once
#include <stdint.h>
#include "imagewriter.h"
#include "halftone.h"

typedef struct {
    const char *version;      // "v0.2.0-29"
    const char *built;        // "Oct 20 2026"
    const char *model;        // the printer model, "ImageWriter II"
    const char *name;         // the name Macs and iPhones see
    const char *host;         // the host name, without .local
    const char *ip;
    const char *setup_ssid;   // in setup mode the open network to join, else NULL
    const char *appletalk;    // the ways in that are on, or "Off"
    iw_ribbon_t ribbon;
    ht_algo_t   screen;       // the screen pages print with
    int         gain;         // its dot gain curve, halftone.h ht_set_gain
} tp_info_t;

typedef struct {
    iw_out_fn out;
    void     *out_ctx;
    // Asked before every line and strip; non-zero stops the page there.
    // NULL never stops.
    int     (*stop)(void *ctx);
    void     *stop_ctx;
} tp_io_t;

typedef enum { TP_DONE = 0, TP_STOPPED = 1, TP_NOMEM = -1 } tp_result_t;

// Sends the whole page, ending with its form feed, unless stopped (then
// mid-page, with no form feed: the caller resyncs the printer as after any
// cut job) or out of memory (as stopped).
tp_result_t tp_print(const tp_info_t *info, const tp_io_t *io);
