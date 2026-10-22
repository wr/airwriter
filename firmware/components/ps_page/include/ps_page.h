// SPDX-License-Identifier: GPL-3.0-or-later
//
// A LaserWriter's page as PostScript (W-1082, laser printers design, "The
// page"): the rip's 1-bit rows as Level 1 imagemask bands. No LaserPrep, no
// filters, no Level 2 operator: plain PostScript that every PostScript
// LaserWriter takes. Pure C; tools/rip builds it on the host, and pscheck.py
// holds it to Ghostscript dot for dot.
//
// Rows gather into a band until a blank row (skipped; it ends the band) or
// PS_BAND_ROWS of them. Each band is trimmed to its first and last byte with
// ink and sent as one imagemask, reading currentfile into a getinterval of
// one prolog string: raw bytes with readstring when the printer takes binary
// (the query job's probe), else hex with readhexstring. A text page comes to
// roughly half its bitmap; a photo barely shrinks.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ripjob.h"

#define PS_BAND_ROWS 64          // 64 x 320 B = 20 KB of band
#define PS_ROW_MAX   320         // bytes: the prolog string; 2560 px at 300 dpi

typedef struct {
    void      (*out)(void *ctx, const uint8_t *buf, size_t n);
    void       *ctx;
    int         binary;          // 1: raw bytes, readstring; 0: hex, readhexstring
    uint32_t    paper_h_pt;      // the paper's height in points: 792, 842, 1008
    const char *title;           // the job's name (UTF-8), escaped into %%Title
    const char *creator;         // "AirWriter 0.3.0"
} ps_cfg_t;

typedef struct ps ps_t;

// NULL when out of memory. The band is allocated here, once per job.
ps_t *ps_new(const ps_cfg_t *cfg);
// The DSC header and the prolog.
void  ps_job_begin(ps_t *ps);
// %%Trailer with the page count, and %%EOF.
void  ps_job_end(ps_t *ps);
void  ps_free(ps_t *ps);
// A rip stage (ripjob.h) that writes each page: its top-left pixel on the
// paper's top-left corner, at 72/300 of a point. Rows wider than PS_ROW_MAX
// bytes lose their right part.
void  ps_stage(ps_t *ps, rip_stage_t *st);
