// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "urf_decode.h"
#include "resample.h"
#include "halftone.h"
#include "imagewriter.h"
#include "png_write.h"

#define DPI_X 144
#define DPI_Y  72

static int file_read(void *ctx, uint8_t *buf, size_t n)
{
    return fread(buf, 1, n, (FILE *)ctx) == n;
}

typedef struct {
    ht_t     ht;
    iw_t     iw;
    FILE    *stream;
    uint8_t *bits;
    uint8_t *preview;      // dst_w * dst_h, grown as rows arrive
    uint32_t dst_w, dst_h, prev_y;
} pipe_t;

static void stream_out(void *ctx, const uint8_t *b, size_t n)
{
    pipe_t *p = (pipe_t *)ctx;
    if (p->stream) fwrite(b, 1, n, p->stream);
}

/* One resampled row: halftone it, feed the printer, record the preview. */
static void on_row(void *ctx, const uint8_t *row, uint32_t w)
{
    pipe_t *p = (pipe_t *)ctx;
    ht_row(&p->ht, row, p->bits);
    iw_row(&p->iw, p->bits);

    if (p->preview && p->prev_y < p->dst_h) {
        uint8_t *dst = p->preview + (size_t)p->prev_y * w;
        for (uint32_t x = 0; x < w; x++)
            dst[x] = (p->bits[x >> 3] & (0x80 >> (x & 7))) ? 0 : 255;
        p->prev_y++;
    }
}

static void usage(void)
{
    fputs("usage: rip --in FILE [--out-stream FILE] [--out-preview FILE]\n"
          "           [--dither threshold|atkinson|floyd|bayer]\n", stderr);
}

int main(int argc, char **argv)
{
    const char *in = NULL, *out_stream = NULL, *out_preview = NULL;
    const char *dither = "atkinson";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc)               in = argv[++i];
        else if (!strcmp(argv[i], "--out-stream") && i + 1 < argc)  out_stream = argv[++i];
        else if (!strcmp(argv[i], "--out-preview") && i + 1 < argc) out_preview = argv[++i];
        else if (!strcmp(argv[i], "--dither") && i + 1 < argc)      dither = argv[++i];
        else { usage(); return 2; }
    }
    if (!in) { usage(); return 2; }

    ht_algo_t algo;
    if (ht_algo_from_name(dither, &algo)) {
        fprintf(stderr, "rip: unknown dither '%s'\n", dither);
        return 2;
    }

    FILE *f = fopen(in, "rb");
    if (!f) { perror(in); return 1; }

    urf_t u;
    int rc = urf_open(&u, file_read, f);
    if (rc != URF_OK) {
        fprintf(stderr, "rip: %s is not a URF file (%d)\n", in, rc);
        fclose(f);
        return 1;
    }

    pipe_t p;
    memset(&p, 0, sizeof p);
    p.stream = out_stream ? fopen(out_stream, "wb") : NULL;
    if (out_stream && !p.stream) { perror(out_stream); fclose(f); return 1; }

    int pages = 0;
    while ((rc = urf_next_page(&u)) == URF_OK) {
        if (u.page.bits_per_pixel != 8 || u.page.num_colors != 1) {
            fprintf(stderr, "rip: page %d is %u bpp / %u colours; only 8-bit "
                            "grayscale is supported\n",
                    pages + 1, u.page.bits_per_pixel, u.page.num_colors);
            rc = -1;
            break;
        }

        uint32_t res = u.page.res_x ? u.page.res_x : DPI_X;
        p.dst_w = (uint32_t)((uint64_t)u.page.width  * DPI_X / res);
        p.dst_h = (uint32_t)((uint64_t)u.page.height * DPI_Y / res);
        if (p.dst_w > IW_MAX_DOTS) p.dst_w = IW_MAX_DOTS;
        if (!p.dst_w || !p.dst_h) { fprintf(stderr, "rip: empty page\n"); rc = -1; break; }

        p.bits   = calloc((p.dst_w + 7) / 8, 1);
        p.prev_y = 0;
        p.preview = out_preview ? calloc((size_t)p.dst_w * p.dst_h, 1) : NULL;
        if (!p.bits || (out_preview && !p.preview)) { rc = -1; break; }

        if (ht_init(&p.ht, algo, p.dst_w)) { rc = -1; break; }
        iw_init(&p.iw, p.dst_w, stream_out, &p);
        iw_begin_page(&p.iw);

        rs_t rs;
        if (rs_init(&rs, u.page.width, u.page.height, p.dst_w, p.dst_h, on_row, &p)) {
            rc = -1; break;
        }

        const uint8_t *row;
        int r;
        while ((r = urf_next_row(&u, &row)) == URF_OK) rs_push(&rs, row);
        if (r < 0) { fprintf(stderr, "rip: decode failed on page %d (%d)\n", pages + 1, r); rc = r; }

        iw_end_page(&p.iw);
        rs_free(&rs);
        ht_free(&p.ht);

        if (p.preview && pages == 0) {
            if (png_write_gray(out_preview, p.preview, p.dst_w, p.dst_h))
                fprintf(stderr, "rip: could not write %s\n", out_preview);
        }
        free(p.preview); p.preview = NULL;
        free(p.bits);    p.bits    = NULL;

        pages++;
        if (rc < 0) break;
    }

    if (p.stream) fclose(p.stream);
    urf_free(&u);
    fclose(f);

    if (rc < 0) return 1;
    fprintf(stderr, "rip: %d page%s\n", pages, pages == 1 ? "" : "s");
    return 0;
}
