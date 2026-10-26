// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host front end for the raster pipeline. The page loop itself lives in the
// ripjob component so this tool and the firmware run the same code -- which is
// what makes the byte-for-byte regression here mean something for the device.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urf_decode.h"
#include "ripjob.h"
#include "png_write.h"
#include "testpage.h"
#include "printer_caps.h"
#include "ps_page.h"
#include "ps_testpage.h"
#include "lw_query.h"

typedef struct {
    FILE    *stream;
    int      write_failed;
    uint8_t *preview;          // dst_w * dst_h of page 1 (x3 in colour), or NULL
    uint32_t dst_w, dst_h;
    int      colour;           // --colour: the preview is RGB
    uint8_t *pbm;              // --out-pbm: page 1's dots, 1 = ink, or NULL
    int      want_pbm;
    FILE    *ps;               // --ps: the LaserWriter's PostScript
} host_t;

static void ps_out(void *ctx, const uint8_t *b, size_t n)
{
    host_t *h = (host_t *)ctx;
    if (h->ps && fwrite(b, 1, n, h->ps) != n) h->write_failed = 1;
}

/* A LaserWriter's page at 300 dpi on the paper named (W-1082): as the board's
 * would be with that size reported, the rest of its caps unused here. */
static int laser_caps(const char *paper, printer_caps_t *c, uint32_t *paper_h_pt)
{
    static const struct { const char *name, *media; int32_t x, y; uint32_t w_px, h_pt; } k[] = {
        { "letter", "na_letter_8.5x11in", 21590, 27940, 2550,  792 },
        { "a4",     "iso_a4_210x297mm",   21000, 29700, 2480,  842 },
        { "legal",  "na_legal_8.5x14in",  21590, 35560, 2550, 1008 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        if (strcmp(paper, k[i].name)) continue;
        memset(c, 0, sizeof *c);
        c->make_model = "Apple LaserWriter";
        c->device_id  = "MFG:Apple;MDL:LaserWriter;CMD:URF;";
        c->urf_rs     = "RS300";
        c->res_x = c->res_y = c->rip_dpi_x = c->rip_dpi_y = 300;
        c->media      = k[i].media;
        c->media_x    = k[i].x;
        c->media_y    = k[i].y;
        c->max_dots   = k[i].w_px;
        *paper_h_pt   = k[i].h_pt;
        return 0;
    }
    return -1;
}

static int file_read(void *ctx, uint8_t *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ctx) == n;
}

static void stream_out(void *ctx, const uint8_t *b, size_t n)
{
    host_t *h = (host_t *)ctx;
    if (h->stream && fwrite(b, 1, n, h->stream) != n) h->write_failed = 1;
}

static void say(void *ctx, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "rip: %s\n", msg);
}

/* Only page 1 is previewed. ripjob reports the geometry before any row of that
 * page arrives, which is when the buffer can be sized. */
static int on_page(void *ctx, int index, uint32_t dst_w, uint32_t dst_h)
{
    host_t *h = (host_t *)ctx;
    if (index != 0) return 0;

    h->dst_w = dst_w;
    h->dst_h = dst_h;
    if (h->want_pbm && !(h->pbm = calloc((size_t)(dst_w + 7) / 8 * dst_h, 1))) return -1;
    if (h->want_pbm == 2) return 0;            // --out-pbm without --out-preview
    h->preview = calloc((size_t)dst_w * dst_h * (h->colour ? 3 : 1), 1);
    return h->preview ? 0 : -1;
}

/* Page 1's dots as a raw PBM (P4: 1 = black, rows padded to bytes), the
 * rip's own answer that pscheck.py holds Ghostscript's rendering to. */
static int write_pbm(const char *path, const uint8_t *bits, uint32_t w, uint32_t h)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P4\n%u %u\n", (unsigned)w, (unsigned)h);
    size_t n = (size_t)(w + 7) / 8 * h;
    int bad = fwrite(bits, 1, n, f) != n;
    return fclose(f) || bad ? -1 : 0;
}

static void on_row(void *ctx, const uint8_t *bits, uint32_t w, uint32_t y)
{
    host_t *h = (host_t *)ctx;
    if (h->pbm && y < h->dst_h && w == h->dst_w)
        memcpy(h->pbm + (size_t)y * ((w + 7) / 8), bits, (w + 7) / 8);
    if (!h->preview || y >= h->dst_h || w != h->dst_w) return;

    uint32_t ch = h->colour ? 3 : 1;
    uint8_t *dst = h->preview + (size_t)y * w * ch;
    for (uint32_t x = 0; x < w; x++)
        for (uint32_t c = 0; c < ch; c++)
            dst[x * ch + c] = (bits[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
}

/* A colour page's preview: each ink's dot takes away what that ink absorbs
 * from white, so overlapping dots mix the way they do on paper. A gray page
 * ripped with --colour still previews through on_row, as gray in RGB. */
static void on_ink_row(void *ctx, const uint8_t *const bits[IW_INKS], uint32_t w, uint32_t y)
{
    host_t *h = (host_t *)ctx;
    if (!h->preview || y >= h->dst_h || w != h->dst_w) return;

    uint8_t *dst = h->preview + (size_t)y * w * 3;
    for (uint32_t x = 0; x < w; x++, dst += 3) {
        int on[IW_INKS];
        for (int i = 0; i < IW_INKS; i++) on[i] = !!(bits[i][x >> 3] & (0x80 >> (x & 7)));
        dst[0] = on[IW_BLACK] || on[IW_CYAN]    ? 0 : 255;
        dst[1] = on[IW_BLACK] || on[IW_MAGENTA] ? 0 : 255;
        dst[2] = on[IW_BLACK] || on[IW_YELLOW]  ? 0 : 255;
    }
}

/* The test page with board 1's details, for tools/iwview to draw. */
static int print_testpage(const char *ribbon, const char *path, ht_algo_t algo, int gain, int top)
{
    tp_info_t info = {
        .version = "v0.2.0-29", .built = "Oct 20 2026", .model = "ImageWriter II",
        .name = "ImageWriter II", .host = "imagewriter-2434", .ip = "10.0.1.115",
        .appletalk = "AppleTalk port", .screen = algo, .gain = gain, .top_margin = top,
        .ribbon = !strcmp(ribbon, "colour") ? IW_RIBBON_COLOUR
                : !strcmp(ribbon, "black")  ? IW_RIBBON_BLACK : IW_RIBBON_UNKNOWN,
    };
    host_t h;
    memset(&h, 0, sizeof h);
    if (!(h.stream = fopen(path, "wb"))) { perror(path); return 1; }
    tp_io_t io = { .out = stream_out, .out_ctx = &h };
    tp_result_t rc = tp_print(&info, &io);
    long n = ftell(h.stream);
    fclose(h.stream);
    if (rc != TP_DONE || h.write_failed) {
        fprintf(stderr, "rip: test page failed (%d)\n", rc);
        return 1;
    }
    fprintf(stderr, "rip: test page, %ld bytes\n", n);
    return 0;
}

static void write_file(void *ctx, const uint8_t *b, size_t n)
{
    fwrite(b, 1, n, (FILE *)ctx);
}

static void usage(void)
{
    fputs("usage: rip --in FILE [--out-stream FILE] [--out-preview FILE]\n"
          "           [--dither threshold|atkinson|floyd|bayer|bluenoise|apple] [--colour]\n"
          "           [--gain PERCENT] [--top-margin N] [--out-pbm FILE]\n"
          "       rip --in FILE --printer laser [--paper letter|a4|legal] [--ps FILE] [--hex]\n"
          "       rip --testpage colour|black|unknown --out-stream FILE [--dither ...] [--gain ...]\n"
          "  --colour   print RGB pages in four inks, as with the colour ribbon in\n"
          "  --gain     dot gain at a 50%% tint the tone curve makes up for (default 0, off)\n"
          "  --top-margin  the board's top margin setting, 1/144 in: + adds paper ahead of\n"
          "             each page, - drops its first rows (the board's default is -144)\n"
          "  --testpage the board's test page for that ribbon, as board 1 would print it;\n"
          "             tools/iwview/iwview.py draws the stream\n"
          "  --out-pbm  page 1's dots as a raw PBM, 1 = ink\n"
          "       rip --query-job FILE   the LaserWriter query job, probe and all\n"
          "       rip --start-page-job on|off FILE  the job that sets its start page (W-1099)\n"
          "       rip --ps-testpage FILE  the PostScript printers' test page (W-1088)\n"
          "  --printer laser  rip at 300 dpi for a LaserWriter (W-1082); --ps writes its\n"
          "             PostScript, in binary, or with --hex as a Level 1 machine that\n"
          "             cannot take binary gets it\n",
          stderr);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out_stream = NULL, *out_preview = NULL;
    const char *dither = "atkinson", *testpage = NULL;
    const char *printer = "imagewriter", *paper = "letter", *out_ps = NULL, *out_pbm = NULL;
    const char *query = NULL, *ps_tp = NULL, *sp_job = NULL;
    int sp_on = 0;
    int colour = 0, gain = 0, top = 0, hex = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc)               in = argv[++i];
        else if (!strcmp(argv[i], "--out-stream") && i + 1 < argc)  out_stream = argv[++i];
        else if (!strcmp(argv[i], "--out-preview") && i + 1 < argc) out_preview = argv[++i];
        else if (!strcmp(argv[i], "--dither") && i + 1 < argc)      dither = argv[++i];
        else if (!strcmp(argv[i], "--colour"))                       colour = 1;
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc)        gain = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--testpage") && i + 1 < argc)    testpage = argv[++i];
        else if (!strcmp(argv[i], "--top-margin") && i + 1 < argc)  top = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--printer") && i + 1 < argc)     printer = argv[++i];
        else if (!strcmp(argv[i], "--paper") && i + 1 < argc)       paper = argv[++i];
        else if (!strcmp(argv[i], "--ps") && i + 1 < argc)          out_ps = argv[++i];
        else if (!strcmp(argv[i], "--hex"))                          hex = 1;
        else if (!strcmp(argv[i], "--out-pbm") && i + 1 < argc)     out_pbm = argv[++i];
        else if (!strcmp(argv[i], "--query-job") && i + 1 < argc)   query = argv[++i];
        else if (!strcmp(argv[i], "--start-page-job") && i + 2 < argc) {
            sp_on = !strcmp(argv[++i], "on");
            sp_job = argv[++i];
        }
        else if (!strcmp(argv[i], "--ps-testpage") && i + 1 < argc) ps_tp = argv[++i];
        else { usage(); return 2; }
    }
    if (ps_tp) {
        FILE *tf = fopen(ps_tp, "wb");
        if (!tf) { perror(ps_tp); return 1; }
        ps_tp_info_t ti = { .printer = "LaserWriter II NT", .product = "LaserWriter II NT",
                            .version = "47.0", .host = "airwriter-2434.local",
                            .board = "AirWriter (host build) at 10.0.1.115" };
        int rc = ps_testpage(&ti, write_file, tf);
        fclose(tf);
        return rc ? 1 : 0;
    }
    if (query || sp_job) {
        static uint8_t q[4096];
        size_t n = query ? lw_query_job(q, sizeof q) : lw_start_page_job(q, sizeof q, sp_on);
        if (!query) query = sp_job;
        FILE *qf = fopen(query, "wb");
        if (!qf || fwrite(q, 1, n, qf) != n) { perror(query); return 1; }
        fclose(qf);
        return 0;
    }
    if (!in && !(testpage && out_stream)) { usage(); return 2; }

    ht_algo_t algo;
    if (ht_algo_from_name(dither, &algo)) {
        fprintf(stderr, "rip: unknown dither '%s'\n", dither);
        return 2;
    }
    if (testpage) return print_testpage(testpage, out_stream, algo, gain, top);

    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }

    urf_t u;
    int rc = urf_open(&u, file_read, f);
    if (rc != URF_OK) {
        if (rc == URF_ERR_UNSUPPORTED && u.is_pwg)
            fprintf(stderr, "rip: %s is PWG raster, which is not supported yet\n", in);
        else if (rc == URF_ERR_MAGIC)
            fprintf(stderr, "rip: %s is neither a URF nor PWG raster file\n", in);
        else
            fprintf(stderr, "rip: could not read %s (%d)\n", in, rc);
        fclose(f);
        return 1;
    }

    host_t h;
    memset(&h, 0, sizeof h);
    h.colour = colour;
    h.want_pbm = out_pbm ? (out_preview ? 1 : 2) : 0;
    h.stream = out_stream ? fopen(out_stream, "wb") : NULL;
    if (out_stream && !h.stream) { perror(out_stream); fclose(f); return 1; }

    int laser = !strcmp(printer, "laser");
    if (!laser && strcmp(printer, "imagewriter")) { usage(); fclose(f); return 2; }
    printer_caps_t caps;
    uint32_t paper_h_pt = 792;
    ps_t *ps = NULL;
    rip_stage_t stage;
    if (laser) {
        if (laser_caps(paper, &caps, &paper_h_pt)) { usage(); fclose(f); return 2; }
        h.ps = out_ps ? fopen(out_ps, "wb") : NULL;
        if (out_ps && !h.ps) { perror(out_ps); fclose(f); return 1; }
        ps_cfg_t pc = { .out = ps_out, .ctx = &h, .binary = !hex, .paper_h_pt = paper_h_pt,
                        .title = in, .creator = "AirWriter rip" };
        if (!(ps = ps_new(&pc))) { fclose(f); return 1; }
        ps_stage(ps, &stage);
        ps_job_begin(ps);
    }

    ripjob_cfg_t cfg = {
        .out      = stream_out,
        .out_ctx  = &h,
        .dither   = algo,
        .gain     = gain,
        .colour   = colour,
        .top_margin = top,
        .caps     = laser ? &caps : NULL,
        .stage    = laser ? &stage : NULL,
        .page     = out_preview || out_pbm ? on_page : NULL,
        .page_ctx = &h,
        .row      = out_preview || out_pbm ? on_row : NULL,
        .row_ctx  = &h,
        .ink_row  = out_preview ? on_ink_row : NULL,
        .log      = say,
    };

    ripjob_stats_t st;
    memset(&st, 0, sizeof st);

    rc = ripjob_run(&u, &cfg, &st);
    if (ps) {
        ps_job_end(ps);
        ps_free(ps);
        if (h.ps) fclose(h.ps);
    }
    if (h.pbm && write_pbm(out_pbm, h.pbm, h.dst_w, h.dst_h))
        fprintf(stderr, "rip: could not write %s\n", out_pbm);

    if (h.preview && (colour ? png_write_rgb(out_preview, h.preview, h.dst_w, h.dst_h)
                             : png_write_gray(out_preview, h.preview, h.dst_w, h.dst_h)))
        fprintf(stderr, "rip: could not write %s\n", out_preview);

    if (h.stream) fclose(h.stream);
    if (h.write_failed) {
        fprintf(stderr, "rip: short write to %s\n", out_stream);
        rc = -1;
    }
    free(h.preview);
    free(h.pbm);
    urf_free(&u);
    fclose(f);

    if (rc < 0) return 1;
    fprintf(stderr, "rip: %d page%s\n", st.pages, st.pages == 1 ? "" : "s");
    return 0;
}
