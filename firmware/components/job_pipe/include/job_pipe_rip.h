// SPDX-License-Identifier: GPL-3.0-or-later
// The rip's settings for one job: pure, so the host tests hold it.
#pragma once
#include "halftone.h"
#include "ripjob.h"

// W-1082: a document for another printer than the ImageWriter: its geometry,
// the output stage its rows go to (a LaserWriter's PostScript, a StyleWriter's
// bands), and its own screen. The top margin and cfg.out are the ImageWriter's
// and unused. The caps and stage must last until cfg.done. Fill it from zero:
// a field a printer has no use for (colour, buffered, prn, tone) is 0 or NULL.
typedef struct {
    const printer_caps_t *caps;
    const rip_stage_t    *stage;
    ht_algo_t             dither;
    int                   gain;
    // W-1103: the page's colour (an RGB page reaches the stage as four planes
    // when the stage has ink_row), the printer's tone curve (ripjob_cfg_t.tone;
    // it must last until cfg.done), and its printer bytes. A LaserWriter has
    // none of these: its stage sends its own bytes, and the whole pool is URF.
    int                   colour;
    // The pool splits as an ImageWriter's does: URF in the first half and
    // printer bytes in the second, which a printer task hands to `prn` while
    // the rip runs ahead of it. The stage writes them with job_pipe_out().
    // Without PSRAM there is no second half, and they go to `prn` as written.
    int                   buffered;
    // Where the job's printer bytes are handed, in chunks of whatever size the
    // ring's printer task reads (up to 512 bytes) or, with no ring, the stage
    // wrote: a consumer that needs whole frames puts them together. NULL takes
    // cfg.out, the ImageWriter's.
    void                (*prn)(void *ctx, const uint8_t *buf, size_t n);
    void                 *prn_ctx;
    const uint8_t        *tone;
} job_pipe_target_t;

// `base` with this job's printer in it. An ImageWriter job (t NULL) asks
// colour_ok (the ImageWriter's ribbon, W-1064); another printer's job asks
// the ImageWriter nothing, since it may be off (W-1082 bench: a LaserWriter
// job waited 10 minutes on a switched-off ImageWriter), and takes its colour
// and tone from its target.
ripjob_cfg_t job_pipe_rip_cfg(ripjob_cfg_t base, int (*colour_ok)(void *ctx), void *colour_ctx,
                              const job_pipe_target_t *t);

// Does this job's printer bytes go through the printer-bytes ring? An
// ImageWriter's do whenever there is one (`have_ring`); another printer's only
// when it asks (`buffered`).
int job_pipe_buffered(const job_pipe_target_t *t, int have_ring);

// A consumer of printer bytes and its context.
typedef struct {
    void (*fn)(void *ctx, const uint8_t *buf, size_t n);
    void  *ctx;
} job_pipe_prn_t;

// Who gets this job's printer bytes: the target's `prn` when it has one, else
// the ImageWriter's cfg.out (`out`, `out_ctx`). fn is NULL when neither is set.
job_pipe_prn_t job_pipe_prn_for(const job_pipe_target_t *t,
                                void (*out)(void *ctx, const uint8_t *buf, size_t n),
                                void *out_ctx);
