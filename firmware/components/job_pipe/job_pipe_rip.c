// SPDX-License-Identifier: GPL-3.0-or-later
#include "job_pipe_rip.h"

ripjob_cfg_t job_pipe_rip_cfg(ripjob_cfg_t base, int (*colour_ok)(void *ctx), void *colour_ctx,
                              const job_pipe_target_t *t)
{
    if (!t) {
        base.colour = colour_ok ? colour_ok(colour_ctx) : 0;
        return base;
    }
    base.caps       = t->caps;
    base.stage      = t->stage;
    base.dither     = t->dither;
    base.gain       = t->gain;
    base.colour     = 0;
    base.top_margin = 0;
    return base;
}
