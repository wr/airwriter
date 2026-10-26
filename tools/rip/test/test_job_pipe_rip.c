// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rip's settings for one job (W-1082). An ImageWriter job asks the
// ImageWriter its ribbon as its rip starts; a LaserWriter's job must not:
// on board 1 the ask waited out a switched-off ImageWriter for 10 minutes
// before the LaserWriter got its first page byte.
#include "test_util.h"
#include "job_pipe_rip.h"
#include "printer_caps.h"

static int g_asked;
static int ask_colour(void *ctx)
{
    (void)ctx;
    g_asked++;
    return 1;
}

static void out_nop(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; (void)n; }

static int  stage_begin(void *ctx, int i, uint32_t w, uint32_t h) { (void)ctx; (void)i; (void)w; (void)h; return 0; }
static void stage_row(void *ctx, const uint8_t *bits) { (void)ctx; (void)bits; }
static int  stage_end(void *ctx) { (void)ctx; return 0; }

int main(void)
{
    ripjob_cfg_t base = { .out = out_nop, .dither = HT_ATKINSON, .gain = 10, .top_margin = -144 };

    // The ImageWriter: its ribbon is asked once, and the rest is its own.
    g_asked = 0;
    ripjob_cfg_t iw = job_pipe_rip_cfg(base, ask_colour, NULL, NULL);
    CHECK_EQ_INT(g_asked, 1);
    CHECK_EQ_INT(iw.colour, 1);
    CHECK_EQ_INT(iw.top_margin, -144);
    CHECK_EQ_INT(iw.gain, 10);
    CHECK(iw.caps == NULL && iw.stage == NULL);

    // No colour_ok: gray, and nothing asked.
    g_asked = 0;
    ripjob_cfg_t gray = job_pipe_rip_cfg(base, NULL, NULL, NULL);
    CHECK_EQ_INT(gray.colour, 0);

    // A LaserWriter's job: the ImageWriter is never asked; the target's
    // geometry, stage and screen; no colour, no top margin.
    static const printer_caps_t caps = { .res_x = 300, .res_y = 300, .max_dots = 2550 };
    static const rip_stage_t stage = { stage_begin, stage_row, stage_end, NULL, NULL };
    job_pipe_target_t t = { .caps = &caps, .stage = &stage, .dither = HT_BLUENOISE, .gain = 0 };
    g_asked = 0;
    ripjob_cfg_t lw = job_pipe_rip_cfg(base, ask_colour, NULL, &t);
    CHECK_EQ_INT(g_asked, 0);
    CHECK_EQ_INT(lw.colour, 0);
    CHECK_EQ_INT(lw.top_margin, 0);
    CHECK(lw.caps == &caps);
    CHECK(lw.stage == &stage);
    CHECK_EQ_INT(lw.dither, HT_BLUENOISE);
    CHECK_EQ_INT(lw.gain, 0);

    TEST_END;
}
