// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rip's settings for one job (W-1082). An ImageWriter job asks the
// ImageWriter its ribbon as its rip starts; a LaserWriter's job must not:
// on board 1 the ask waited out a switched-off ImageWriter for 10 minutes
// before the LaserWriter got its first page byte. A StyleWriter's job (W-1103)
// takes colour, a tone curve and the printer-bytes ring from its target.
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

// A target's colour_ok: fills in the stage it is given (the board makes it here) and says
// whether RGB pages print in colour.
static int g_target_asked, g_target_says;
static int  stage_begin(void *ctx, int i, uint32_t w, uint32_t h);
static void stage_row(void *ctx, const uint8_t *bits);
static int  stage_end(void *ctx);
static int target_colour(void *ctx)
{
    g_target_asked++;
    rip_stage_t *st = ctx;
    st->page_begin = stage_begin;
    st->row = stage_row;
    st->page_end = stage_end;
    return g_target_says;
}

static int ink_rows, gray_rows, begins;
static int  cnt_begin(void *ctx, int i, uint32_t w, uint32_t h) { (void)ctx; (void)i; (void)w; (void)h; begins++; return 0; }
static void cnt_row(void *ctx, const uint8_t *bits) { (void)ctx; (void)bits; gray_rows++; }
static void cnt_ink_row(void *ctx, const uint8_t *const bits[IW_INKS]) { (void)ctx; (void)bits; ink_rows++; }

static uint8_t g_page[1 << 14];

// A one-page URF, w x h at 300 dpi: 24-bit sRGB (a flat orange) or 8-bit gray.
static size_t make_page(int rgb, uint32_t w, uint32_t h)
{
    size_t n = 0;
    memcpy(g_page, "UNIRAST\0", 8); n = 8;
    g_page[n++] = 0; g_page[n++] = 0; g_page[n++] = 0; g_page[n++] = 1;
    uint8_t hdr[32] = { 0 };
    hdr[0] = rgb ? 24 : 8;
    hdr[1] = rgb ? 1 : 0;
    hdr[12] = (uint8_t)(w >> 24); hdr[13] = (uint8_t)(w >> 16); hdr[14] = (uint8_t)(w >> 8); hdr[15] = (uint8_t)w;
    hdr[16] = (uint8_t)(h >> 24); hdr[17] = (uint8_t)(h >> 16); hdr[18] = (uint8_t)(h >> 8); hdr[19] = (uint8_t)h;
    hdr[22] = (uint8_t)(300 >> 8); hdr[23] = (uint8_t)(300 & 255);
    memcpy(g_page + n, hdr, 32); n += 32;
    for (uint32_t y = 0; y < h; y++) {
        g_page[n++] = 0;                                  // one line
        for (uint32_t x = 0; x < w; x++) {                // every pixel a literal
            g_page[n++] = 0;                              // a count of 1: one pixel follows
            if (rgb) { g_page[n++] = 230; g_page[n++] = 120; g_page[n++] = 30; }
            else     { g_page[n++] = 128; }
        }
    }
    return n;
}

static int rip_page(const uint8_t *urf, size_t len, ripjob_cfg_t *cfg)
{
    memsrc_t m = { urf, len, 0 };
    urf_t u;
    if (urf_open(&u, memsrc_read, &m) != URF_OK) return -100;
    int rc = ripjob_run(&u, cfg, NULL);
    urf_free(&u);
    return rc;
}

static void out_nop(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; (void)n; }
static void prn_nop(void *ctx, const uint8_t *b, size_t n) { (void)ctx; (void)b; (void)n; }

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

    CHECK(lw.tone == NULL);

    // W-1103, a StyleWriter's job: its target's colour and tone curve reach the rip.
    // The ImageWriter's ribbon is still not asked.
    static const uint8_t tone[256] = { 1, 2, 3 };
    static const rip_stage_t ink_stage = { stage_begin, stage_row, stage_end, NULL, cnt_ink_row };
    job_pipe_target_t sw = { .caps = &caps, .stage = &ink_stage, .dither = HT_APPLE, .gain = 0,
                             .colour = 1, .buffered = 1, .tone = tone };
    g_asked = 0;
    ripjob_cfg_t sc = job_pipe_rip_cfg(base, ask_colour, NULL, &sw);
    CHECK_EQ_INT(g_asked, 0);
    CHECK_EQ_INT(sc.colour, 1);
    CHECK(sc.tone == tone);
    CHECK_EQ_INT(sc.top_margin, 0);
    CHECK(sc.stage == &ink_stage);
    CHECK_EQ_INT(sc.dither, HT_APPLE);
    sw.colour = 0;
    CHECK_EQ_INT(job_pipe_rip_cfg(base, ask_colour, NULL, &sw).colour, 0);
    CHECK(job_pipe_rip_cfg(base, ask_colour, NULL, NULL).tone == NULL);   // the ImageWriter has none

    // A target's own colour_ok (a StyleWriter is reset and its cartridge read first) is asked
    // once, on the way to the rip, and its answer is the job's colour in place of `colour`;
    // what it fills in reaches the rip too, because the stage is the target's by pointer.
    // The ImageWriter's is still not asked.
    static rip_stage_t late_stage;
    job_pipe_target_t lt = { .caps = &caps, .stage = &late_stage, .dither = HT_APPLE, .colour = 1,
                             .buffered = 1, .colour_ok = target_colour, .colour_ctx = &late_stage };
    g_asked = g_target_asked = 0;
    g_target_says = 0;
    ripjob_cfg_t lc = job_pipe_rip_cfg(base, ask_colour, NULL, &lt);
    CHECK_EQ_INT(g_target_asked, 1);
    CHECK_EQ_INT(g_asked, 0);
    CHECK_EQ_INT(lc.colour, 0);                       // not `colour` 1: the target's answer
    CHECK(lc.stage == &late_stage && late_stage.page_begin == stage_begin);
    g_target_says = 1;
    CHECK_EQ_INT(job_pipe_rip_cfg(base, ask_colour, NULL, &lt).colour, 1);
    CHECK_EQ_INT(g_target_asked, 2);
    lt.colour = 0;
    CHECK_EQ_INT(job_pipe_rip_cfg(base, ask_colour, NULL, &lt).colour, 1);
    lt.colour_ok = NULL;                              // without one `colour` rules, as before
    g_target_asked = 0;
    CHECK_EQ_INT(job_pipe_rip_cfg(base, ask_colour, NULL, &lt).colour, 0);
    CHECK_EQ_INT(g_target_asked, 0);

    // And the rip runs it that way: an RGB page, to a stage with ink_row, in colour is
    // four planes a row and no gray rows; with colour 0 it is gray rows, as for a
    // LaserWriter; a gray page is gray rows either way. A colour target whose stage
    // takes no ink rows falls back to gray, as ripjob's own tests hold.
    static const printer_caps_t rcaps = { .res_x = 300, .res_y = 300, .rip_dpi_x = 300,
                                          .rip_dpi_y = 300, .max_dots = 2550 };
    static const rip_stage_t gray_stage = { cnt_begin, cnt_row, stage_end, NULL, NULL };
    static const rip_stage_t both_stage = { cnt_begin, cnt_row, stage_end, NULL, cnt_ink_row };
    const uint32_t PW = 40, PH = 12;
    size_t rgb_n = make_page(1, PW, PH);
    uint8_t rgb_urf[sizeof g_page];
    memcpy(rgb_urf, g_page, rgb_n);
    size_t gray_n = make_page(0, PW, PH);

    job_pipe_target_t ct = { .caps = &rcaps, .stage = &both_stage, .dither = HT_APPLE, .colour = 1 };
    ink_rows = gray_rows = begins = 0;
    ripjob_cfg_t cc = job_pipe_rip_cfg(base, ask_colour, NULL, &ct);
    CHECK_EQ_INT(rip_page(rgb_urf, rgb_n, &cc), 0);
    CHECK_EQ_INT(begins, 1);
    CHECK_EQ_INT(ink_rows, (int)PH);
    CHECK_EQ_INT(gray_rows, 0);

    ct.colour = 0;
    ink_rows = gray_rows = begins = 0;
    cc = job_pipe_rip_cfg(base, ask_colour, NULL, &ct);
    CHECK_EQ_INT(rip_page(rgb_urf, rgb_n, &cc), 0);
    CHECK_EQ_INT(ink_rows, 0);
    CHECK_EQ_INT(gray_rows, (int)PH);

    ct.colour = 1;
    ink_rows = gray_rows = begins = 0;
    cc = job_pipe_rip_cfg(base, ask_colour, NULL, &ct);
    CHECK_EQ_INT(rip_page(g_page, gray_n, &cc), 0);
    CHECK_EQ_INT(ink_rows, 0);
    CHECK_EQ_INT(gray_rows, (int)PH);

    ct.stage = &gray_stage;
    ink_rows = gray_rows = begins = 0;
    cc = job_pipe_rip_cfg(base, ask_colour, NULL, &ct);
    CHECK_EQ_INT(rip_page(rgb_urf, rgb_n, &cc), 0);
    CHECK_EQ_INT(ink_rows, 0);
    CHECK_EQ_INT(gray_rows, (int)PH);

    // Which of the pool's two layouts a job takes, and where its printer's bytes go.
    // The ImageWriter's splits it whenever there is a printer-bytes ring; another
    // printer's only when it asks to (a LaserWriter's rows go to its own stage and
    // keep the whole pool for URF); with no ring, no job does.
    job_pipe_target_t lwt = { .caps = &caps, .stage = &stage };
    job_pipe_target_t swt = { .caps = &caps, .stage = &ink_stage, .buffered = 1 };
    CHECK_EQ_INT(job_pipe_buffered(NULL, 1), 1);
    CHECK_EQ_INT(job_pipe_buffered(NULL, 0), 0);
    CHECK_EQ_INT(job_pipe_buffered(&lwt, 1), 0);
    CHECK_EQ_INT(job_pipe_buffered(&swt, 1), 1);
    CHECK_EQ_INT(job_pipe_buffered(&swt, 0), 0);

    int prn_ctx_v, out_ctx_v;
    job_pipe_prn_t p = job_pipe_prn_for(NULL, out_nop, &out_ctx_v);        // the ImageWriter: cfg.out
    CHECK(p.fn == out_nop && p.ctx == &out_ctx_v);
    p = job_pipe_prn_for(&lwt, out_nop, &out_ctx_v);                       // no consumer: cfg.out as ever
    CHECK(p.fn == out_nop && p.ctx == &out_ctx_v);
    swt.prn = prn_nop;
    swt.prn_ctx = &prn_ctx_v;
    p = job_pipe_prn_for(&swt, out_nop, &out_ctx_v);                       // the target's own
    CHECK(p.fn == prn_nop && p.ctx == &prn_ctx_v);
    p = job_pipe_prn_for(&swt, NULL, NULL);
    CHECK(p.fn == prn_nop && p.ctx == &prn_ctx_v);
    p = job_pipe_prn_for(NULL, NULL, NULL);                                // nothing set: nothing to call
    CHECK(p.fn == NULL);

    TEST_END;
}
