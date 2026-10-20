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

typedef struct {
    FILE    *stream;
    int      write_failed;
    uint8_t *preview;          // dst_w * dst_h of page 1 (x3 in colour), or NULL
    uint32_t dst_w, dst_h;
    int      colour;           // --colour: the preview is RGB
} host_t;

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
    h->preview = calloc((size_t)dst_w * dst_h * (h->colour ? 3 : 1), 1);
    return h->preview ? 0 : -1;
}

static void on_row(void *ctx, const uint8_t *bits, uint32_t w, uint32_t y)
{
    host_t *h = (host_t *)ctx;
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
static int print_testpage(const char *ribbon, const char *path, ht_algo_t algo, int gain)
{
    tp_info_t info = {
        .version = "v0.2.0-29", .built = "Oct 20 2026", .model = "ImageWriter II",
        .name = "ImageWriter II", .host = "imagewriter-2434", .ip = "10.0.1.115",
        .appletalk = "AppleTalk port", .screen = algo, .gain = gain,
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

static void usage(void)
{
    fputs("usage: rip --in FILE [--out-stream FILE] [--out-preview FILE]\n"
          "           [--dither threshold|atkinson|floyd|bayer|bluenoise] [--colour]\n"
          "           [--gain PERCENT]\n"
          "       rip --testpage colour|black|unknown --out-stream FILE [--dither ...] [--gain ...]\n"
          "  --colour   print RGB pages in four inks, as with the colour ribbon in\n"
          "  --gain     dot gain at a 50%% tint the tone curve makes up for (default 0, off)\n"
          "  --testpage the board's test page for that ribbon, as board 1 would print it;\n"
          "             tools/iwview/iwview.py draws the stream\n",
          stderr);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out_stream = NULL, *out_preview = NULL;
    const char *dither = "atkinson", *testpage = NULL;
    int colour = 0, gain = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc)               in = argv[++i];
        else if (!strcmp(argv[i], "--out-stream") && i + 1 < argc)  out_stream = argv[++i];
        else if (!strcmp(argv[i], "--out-preview") && i + 1 < argc) out_preview = argv[++i];
        else if (!strcmp(argv[i], "--dither") && i + 1 < argc)      dither = argv[++i];
        else if (!strcmp(argv[i], "--colour"))                       colour = 1;
        else if (!strcmp(argv[i], "--gain") && i + 1 < argc)        gain = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--testpage") && i + 1 < argc)    testpage = argv[++i];
        else { usage(); return 2; }
    }
    if (!in && !(testpage && out_stream)) { usage(); return 2; }

    ht_algo_t algo;
    if (ht_algo_from_name(dither, &algo)) {
        fprintf(stderr, "rip: unknown dither '%s'\n", dither);
        return 2;
    }
    if (testpage) return print_testpage(testpage, out_stream, algo, gain);

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
    h.stream = out_stream ? fopen(out_stream, "wb") : NULL;
    if (out_stream && !h.stream) { perror(out_stream); fclose(f); return 1; }

    ripjob_cfg_t cfg = {
        .out      = stream_out,
        .out_ctx  = &h,
        .dither   = algo,
        .gain     = gain,
        .colour   = colour,
        .page     = out_preview ? on_page : NULL,
        .page_ctx = &h,
        .row      = out_preview ? on_row : NULL,
        .row_ctx  = &h,
        .ink_row  = out_preview ? on_ink_row : NULL,
        .log      = say,
    };

    ripjob_stats_t st;
    memset(&st, 0, sizeof st);

    rc = ripjob_run(&u, &cfg, &st);

    if (h.preview && (colour ? png_write_rgb(out_preview, h.preview, h.dst_w, h.dst_h)
                             : png_write_gray(out_preview, h.preview, h.dst_w, h.dst_h)))
        fprintf(stderr, "rip: could not write %s\n", out_preview);

    if (h.stream) fclose(h.stream);
    if (h.write_failed) {
        fprintf(stderr, "rip: short write to %s\n", out_stream);
        rc = -1;
    }
    free(h.preview);
    urf_free(&u);
    fclose(f);

    if (rc < 0) return 1;
    fprintf(stderr, "rip: %d page%s\n", st.pages, st.pages == 1 ? "" : "s");
    return 0;
}
