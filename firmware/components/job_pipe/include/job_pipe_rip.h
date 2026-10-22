// SPDX-License-Identifier: GPL-3.0-or-later
// The rip's settings for one job: pure, so the host tests hold it.
#pragma once
#include "halftone.h"
#include "ripjob.h"

// W-1082: a document for another printer than the ImageWriter: its geometry,
// the output stage its rows go to (a LaserWriter's PostScript), and its own
// screen. The colour path, the top margin and cfg.out are the ImageWriter's
// and unused. The caps and stage must last until cfg.done.
typedef struct {
    const printer_caps_t *caps;
    const rip_stage_t    *stage;
    ht_algo_t             dither;
    int                   gain;
} job_pipe_target_t;

// `base` with this job's printer in it. An ImageWriter job (t NULL) asks
// colour_ok (the ImageWriter's ribbon, W-1064); another printer's job asks
// the ImageWriter nothing, since it may be off (W-1082 bench: a LaserWriter
// job waited 10 minutes on a switched-off ImageWriter).
ripjob_cfg_t job_pipe_rip_cfg(ripjob_cfg_t base, int (*colour_ok)(void *ctx), void *colour_ctx,
                              const job_pipe_target_t *t);
