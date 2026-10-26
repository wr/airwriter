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
    base.colour     = t->colour;
    base.tone       = t->tone;
    base.top_margin = 0;
    return base;
}

int job_pipe_buffered(const job_pipe_target_t *t, int have_ring)
{
    return have_ring && (!t || t->buffered);
}

job_pipe_prn_t job_pipe_prn_for(const job_pipe_target_t *t,
                                void (*out)(void *ctx, const uint8_t *buf, size_t n),
                                void *out_ctx)
{
    if (t && t->prn) return (job_pipe_prn_t){ t->prn, t->prn_ctx };
    return (job_pipe_prn_t){ out, out_ctx };
}
