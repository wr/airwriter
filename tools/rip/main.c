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
#include "sw_page.h"
#include "sw_rip.h"
#include "sw_testpage.h"
#include "qd_page.h"
#include "qd_rip.h"
#include "qd_session.h"
#include "qd_wire.h"
#include "qd_testpage.h"

typedef struct {
    FILE    *stream;
    int      write_failed;
    uint8_t *preview;          // dst_w * dst_h of page 1 (x3 in colour), or NULL
    uint32_t dst_w, dst_h;
    int      colour;           // --colour: the preview is RGB
    uint8_t *pbm;              // --out-pbm: page 1's dots, 1 = ink, or NULL
    int      want_pbm;
    FILE    *ps;               // --ps: the LaserWriter's PostScript
    FILE    *sw;               // --out-sw: a StyleWriter's page, as it goes down the wire
    int      sw_model;         // --printer stylewriter: --out-pbm is a file per ink
    uint8_t *swp[IW_INKS];     // page 1's four planes (by iw_ink_t), whole rows
} host_t;

static void ps_out(void *ctx, const uint8_t *b, size_t n)
{
    host_t *h = (host_t *)ctx;
    if (h->ps && fwrite(b, 1, n, h->ps) != n) h->write_failed = 1;
}

/* The page part of the wire stream swfake.py decode reads (W-1103): each rect as
 * its bytes, each block as G + length + data + 00, 0C at a page's end. The
 * session's setup letters and queries are not here; sw_page's frames are
 * [type][u16 LE length][payload]. */
static void sw_out(void *ctx, const uint8_t *f, size_t n)
{
    host_t *h = (host_t *)ctx;
    size_t len = (size_t)(f[1] | (f[2] << 8));
    if (!h->sw || n != 3 + len) return;
    const uint8_t *pl = f + 3;
    int bad = 0;
    switch (f[0]) {
    case SWF_RECT:
        bad = fwrite(pl, 1, len, h->sw) != len;
        break;
    case SWF_BLOCK: {
        uint8_t head[3] = { 'G', f[1], f[2] }, tail = 0;
        bad = fwrite(head, 1, 3, h->sw) != 3 || fwrite(pl, 1, len, h->sw) != len ||
              fwrite(&tail, 1, 1, h->sw) != 1;
        break;
    }
    case SWF_PAGE_END: {
        uint8_t ff = 0x0C;
        bad = fwrite(&ff, 1, 1, h->sw) != 1;
        break;
    }
    default:                                    /* SWF_PAGE: the session's, not the wire's */
        break;
    }
    if (bad) h->write_failed = 1;
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
    if (h->want_pbm && h->sw_model) {
        for (int i = 0; i < IW_INKS; i++)
            if (!(h->swp[i] = calloc((size_t)(dst_w + 7) / 8 * dst_h, 1))) return -1;
    } else if (h->want_pbm && !(h->pbm = calloc((size_t)(dst_w + 7) / 8 * dst_h, 1))) {
        return -1;
    }
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

/* --out-pbm for a StyleWriter: a PBM per ink, "p.pbm" becoming p-c.pbm, p-m.pbm,
 * p-y.pbm and p-k.pbm. Each holds the region the page stage keeps and the printer
 * prints, the same one swfake.py decode writes (its pages' printable area): the
 * rows between the engine's top and bottom margins, the columns the rip kept
 * (swcheck.py pads a page narrower than the engine's width with white). */
static int write_sw_pbms(const char *path, host_t *h, const sw_spec_t *spec)
{
    static const struct { iw_ink_t ink; char letter; } k[] = {
        { IW_CYAN, 'c' }, { IW_MAGENTA, 'm' }, { IW_YELLOW, 'y' }, { IW_BLACK, 'k' },
    };
    size_t n = strlen(path), stem = n > 4 && !strcmp(path + n - 4, ".pbm") ? n - 4 : n;
    uint32_t top = spec->margin_t, bottom = spec->margin_b;
    uint32_t rows = h->dst_h > top + bottom ? h->dst_h - top - bottom : 0;
    size_t rb = (h->dst_w + 7) / 8;
    int bad = 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        char name[1024];
        snprintf(name, sizeof name, "%.*s-%c.pbm", (int)stem, path, k[i].letter);
        if (!h->swp[k[i].ink] || write_pbm(name, h->swp[k[i].ink] + top * rb, h->dst_w, rows)) {
            fprintf(stderr, "rip: could not write %s\n", name);
            bad = 1;
        }
    }
    return bad;
}

static void on_row(void *ctx, const uint8_t *bits, uint32_t w, uint32_t y)
{
    host_t *h = (host_t *)ctx;
    if (h->pbm && y < h->dst_h && w == h->dst_w)
        memcpy(h->pbm + (size_t)y * ((w + 7) / 8), bits, (w + 7) / 8);
    if (h->swp[IW_BLACK] && y < h->dst_h && w == h->dst_w)      /* a gray page: the K plane alone */
        memcpy(h->swp[IW_BLACK] + (size_t)y * ((w + 7) / 8), bits, (w + 7) / 8);
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
    if (y >= h->dst_h || w != h->dst_w) return;
    for (int i = 0; i < IW_INKS; i++)
        if (h->swp[i]) memcpy(h->swp[i] + (size_t)y * ((w + 7) / 8), bits[i], (w + 7) / 8);
    if (!h->preview) return;

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

/* A StyleWriter's test page, drawn as the board draws it for `model` with a black or colour
 * cartridge, written as the wire stream swfake.py decode reads (--out-sw). */
static int print_sw_testpage(const char *cart, sw_model_t model, const char *path, ht_algo_t algo,
                             int gain)
{
    int colour = !strcmp(cart, "colour");
    if (colour && !sw_spec(model)->colour) {
        fprintf(stderr, "rip: the %s takes no colour cartridge\n", sw_spec(model)->name);
        return 2;
    }
    host_t h;
    memset(&h, 0, sizeof h);
    if (!(h.sw = fopen(path, "wb"))) { perror(path); return 1; }
    sw_page_cfg_t pc = { .model = model, .colour_cart = colour, .out = sw_out, .ctx = &h };
    sw_page_t *p = sw_page_new(&pc);
    rip_stage_t st;
    int rc = -1;
    if (p) {
        sw_page_stage(p, &st);
        sw_tp_info_t info = {
            .printer = sw_spec(model)->name, .board = "AirWriter v0.3.0 at 10.0.1.115",
            .host = "airwriter-2434.local", .model = model, .colour_cart = colour, .gain = gain,
            .screen = algo,
        };
        rc = sw_testpage(&info, &st);
        sw_page_free(p);
    }
    long n = ftell(h.sw);
    fclose(h.sw);
    if (rc != SW_TP_DONE || h.write_failed) {
        fprintf(stderr, "rip: test page failed (%d)\n", rc);
        return 1;
    }
    fprintf(stderr, "rip: %s test page, %ld bytes\n", sw_spec(model)->name, n);
    return 0;
}

/* --printer plw300 (W-1102): the firmware's session and page stage write the conversation as
 * the pty carries it to qdfake.py (--out-qd: each packet as `P`, a u16 LE length, its bytes)
 * and as the line the board clocks out (--out-qd-bits: qd_bits_put's image). Every question is
 * answered as an idle Personal LaserWriter 300 with paper at both sources answers it, on a
 * clock that moves only when the session waits. */
typedef struct {
    FILE    *frames;
    uint8_t *bits;
    size_t   bits_cap, bits_pos;     // in bits
    int      failed;
    uint8_t  asked[2];               // the question owed a reply
    int      owed;
    uint32_t ms;
    int      page_no;
    ht_algo_t screen;
    qd_session_t *sess;
} qdw_t;

static int qdw_put(void *ctx, const uint8_t *p, size_t n)
{
    qdw_t *w = ctx;
    uint8_t head[3] = { 'P', (uint8_t)n, (uint8_t)(n >> 8) };
    if (w->frames && (fwrite(head, 1, 3, w->frames) != 3 || fwrite(p, 1, n, w->frames) != n))
        w->failed = 1;
    size_t need = w->bits_pos + qd_bits_len(n);
    if (need > w->bits_cap) {
        size_t cap = w->bits_cap ? w->bits_cap : 1 << 20;
        while (cap < need) cap *= 2;
        uint8_t *nb = realloc(w->bits, cap / 8);
        if (!nb) { w->failed = 1; return QD_E_LINK; }
        w->bits = nb;
        w->bits_cap = cap;
    }
    qd_bits_put(w->bits, w->bits_cap, &w->bits_pos, p, n);
    if (n == 3 && (p[0] == 0x82 || p[0] == 0x83 || p[0] == 0x8E)) {
        w->asked[0] = p[0];
        w->asked[1] = p[1];
        w->owed = 1;
    }
    return 0;
}

static int qdw_flush(void *ctx) { (void)ctx; return 0; }

static int qdw_read(void *ctx, uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms)
{
    qdw_t *w = ctx;
    (void)gap_ms;
    if (!w->owed || !cap) { w->ms += first_ms; return 0; }
    w->owed = 0;
    switch (w->asked[0]) {
    case 0x82: b[0] = 0xA4; break;                    // idle, memory empty
    case 0x8E: b[0] = 0xFC; break;                    // page done: the page prints at once
    default:   b[0] = w->asked[1] == 0x3E ? 0x29      // a Personal LaserWriter 300
                    : w->asked[1] == 0x2A ? 0x61      // paper in the cassette and the slot
                    : 0x01; break;                    // nothing to say
    }
    return 1;
}

static void qdw_sleep(void *ctx, uint32_t ms) { ((qdw_t *)ctx)->ms += ms; }
static uint32_t qdw_now(void *ctx) { return ((qdw_t *)ctx)->ms; }

static int qdw_open(void *ctx, const qd_media_t *md) { return qd_session_open(((qdw_t *)ctx)->sess, md); }
static int qdw_row(void *ctx, const uint8_t *c, size_t n) { return qd_session_row(((qdw_t *)ctx)->sess, c, n); }
static int qdw_print(void *ctx) { return qd_session_print(((qdw_t *)ctx)->sess); }
static void qdw_note(void *ctx, int page, int apple, uint32_t bytes, int too_big)
{
    (void)page;
    qdw_t *w = ctx;
    if (too_big) fprintf(stderr, "rip: page %d: didn't fit in the printer's memory\n", ++w->page_no);
    else fprintf(stderr, "rip: page %d: %s, %u bytes\n", ++w->page_no, apple ? "apple" : "record's screen",
                 (unsigned)bytes);
}

static void qdt_note(void *ctx, int page, int apple, uint32_t bytes, int too_big)
{
    (void)ctx; (void)page; (void)apple;
    if (too_big) fprintf(stderr, "rip: test page: didn't fit in the printer's memory\n");
    else fprintf(stderr, "rip: test page: %u bytes of codes\n", (unsigned)bytes);
}

/* --qd-testpage (W-1102): the 300's test page as the board draws it, through the same session
 * and page stage as --printer plw300, its packets written in qdfake.py's framing (--out-qd's).
 * Its rows go in the screen they are drawn in: nothing is kept. */
static int print_qd_testpage(const char *path, ht_algo_t algo, int gain)
{
    qdw_t qw;
    memset(&qw, 0, sizeof qw);
    qw.screen = algo;
    if (!(qw.frames = fopen(path, "wb"))) { perror(path); return 1; }
    static const qd_ops_t qops = { .put = qdw_put, .flush = qdw_flush, .read = qdw_read,
                                   .sleep_ms = qdw_sleep, .now_ms = qdw_now };
    qd_ops_t ops = qops;
    ops.ctx = &qw;
    const qd_job_cfg_t jc = { .model = QD_PLW300 };
    qd_page_cfg_t pc = { .model = QD_PLW300, .screen = algo, .gain = gain,
                         .sink = { qdw_open, qdw_row, qdw_print, qdt_note, &qw } };
    sw_tp_geom_t g;
    qd_page_t *qdp = NULL;
    int rc = -1;
    if (!qd_tp_geom(QD_PLW300, &g) && (qw.sess = qd_session_new(&ops, &jc)) &&
        (qdp = qd_page_new(&pc)) && !qd_session_begin(qw.sess)) {
        rip_stage_t st;
        qd_page_stage(qdp, &st);
        sw_tp_info_t info = {
            .printer = "Personal LaserWriter 300", .board = "AirWriter v0.3.0 at 10.0.1.115",
            .host = "airwriter-2434.local", .gain = gain, .screen = algo, .geom = &g,
        };
        rc = sw_testpage(&info, &st);
        if (rc == SW_TP_DONE && qd_session_end(qw.sess)) rc = -1;
    }
    qd_page_free(qdp);
    qd_session_free(qw.sess);
    if (fclose(qw.frames)) qw.failed = 1;
    free(qw.bits);
    if (rc != SW_TP_DONE || qw.failed) {
        fprintf(stderr, "rip: the 300's test page failed (%d)\n", rc);
        return 1;
    }
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
          "       rip --in FILE --printer stylewriter --model original|ii|1200|1500|2200|2400|2500\n"
          "           [--cart black|colour] [--tone none] [--out-sw FILE] [--out-pbm FILE]\n"
          "       rip --in FILE --printer plw300 [--source main|manual] [--out-qd FILE]\n"
          "           [--out-qd-bits FILE] [--out-pbm FILE]\n"
          "       rip --testpage colour|black|unknown --out-stream FILE [--dither ...] [--gain ...]\n"
          "       rip --sw-testpage black|colour --model M --out-sw FILE [--dither ...] [--gain ...]\n"
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
          "       --sw-testpage  a StyleWriter's test page for that cartridge, as the board draws it\n"
          "       rip --qd-testpage FILE [--dither ...] [--gain ...]  the Personal LaserWriter 300's\n"
          "             test page as the board sends it, in --out-qd's framing\n"
          "             (W-1103); --out-sw writes it as the printer gets it, for swfake.py decode\n"
          "  --printer stylewriter  rip at 360 dpi for a StyleWriter (W-1103), the sheet named by\n"
          "             the page's width. --out-sw writes the page as the printer gets it (rects,\n"
          "             G blocks, 0C; no setup or queries) for tools/stylewriter/swfake.py decode;\n"
          "             --out-pbm writes a PBM per ink (p-c.pbm .. p-k.pbm) of the printable area;\n"
          "             --cart colour prints RGB pages in four inks, --tone none leaves out Apple's\n"
          "             tone curve\n"
          "  --printer plw300  rip at 300 dpi for a Personal LaserWriter 300 (W-1102), through the\n"
          "             firmware's page stage and session: --out-qd writes the packets in qdfake.py's\n"
          "             framing, --out-qd-bits the line the board clocks out, --out-pbm the dots in\n"
          "             the record's screen; --source manual takes the manual feed slot\n"
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
    const char *model_arg = NULL, *cart_arg = "black", *tone_arg = "apple", *out_sw = NULL;
    const char *sw_tp = NULL, *qd_tp = NULL;
    const char *out_qd = NULL, *out_qd_bits = NULL, *source = "main";
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
        else if (!strcmp(argv[i], "--model") && i + 1 < argc)       model_arg = argv[++i];
        else if (!strcmp(argv[i], "--cart") && i + 1 < argc)        cart_arg = argv[++i];
        else if (!strcmp(argv[i], "--tone") && i + 1 < argc)        tone_arg = argv[++i];
        else if (!strcmp(argv[i], "--out-sw") && i + 1 < argc)      out_sw = argv[++i];
        else if (!strcmp(argv[i], "--out-pbm") && i + 1 < argc)     out_pbm = argv[++i];
        else if (!strcmp(argv[i], "--out-qd") && i + 1 < argc)      out_qd = argv[++i];
        else if (!strcmp(argv[i], "--out-qd-bits") && i + 1 < argc) out_qd_bits = argv[++i];
        else if (!strcmp(argv[i], "--source") && i + 1 < argc)      source = argv[++i];
        else if (!strcmp(argv[i], "--query-job") && i + 1 < argc)   query = argv[++i];
        else if (!strcmp(argv[i], "--start-page-job") && i + 2 < argc) {
            sp_on = !strcmp(argv[++i], "on");
            sp_job = argv[++i];
        }
        else if (!strcmp(argv[i], "--ps-testpage") && i + 1 < argc) ps_tp = argv[++i];
        else if (!strcmp(argv[i], "--sw-testpage") && i + 1 < argc) sw_tp = argv[++i];
        else if (!strcmp(argv[i], "--qd-testpage") && i + 1 < argc) qd_tp = argv[++i];
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
    if (!in && !(testpage && out_stream) && !(sw_tp && out_sw) && !qd_tp) { usage(); return 2; }

    ht_algo_t algo;
    if (ht_algo_from_name(dither, &algo)) {
        fprintf(stderr, "rip: unknown dither '%s'\n", dither);
        return 2;
    }
    if (sw_tp) {
        sw_model_t m;
        if (!model_arg || sw_rip_model(model_arg, &m) || (strcmp(sw_tp, "black") && strcmp(sw_tp, "colour"))) {
            usage();
            return 2;
        }
        return print_sw_testpage(sw_tp, m, out_sw, algo, gain);
    }
    if (qd_tp) return print_qd_testpage(qd_tp, algo, gain);
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

    int laser = !strcmp(printer, "laser"), stylewriter = !strcmp(printer, "stylewriter");
    int plw300 = !strcmp(printer, "plw300");
    if (!laser && !stylewriter && !plw300 && strcmp(printer, "imagewriter")) { usage(); fclose(f); return 2; }
    if (plw300 && strcmp(source, "main") && strcmp(source, "manual")) { usage(); fclose(f); return 2; }
    printer_caps_t caps;
    uint32_t paper_h_pt = 792;
    ps_t *ps = NULL;
    sw_page_t *swp = NULL;
    sw_model_t swm = SW_NONE;
    rip_stage_t stage;
    memset(&stage, 0, sizeof stage);
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
    if (stylewriter) {
        if (!model_arg || sw_rip_model(model_arg, &swm) ||
            (strcmp(cart_arg, "black") && strcmp(cart_arg, "colour")) ||
            (strcmp(tone_arg, "apple") && strcmp(tone_arg, "none"))) {
            usage();
            fclose(f);
            return 2;
        }
        int colour_cart = !strcmp(cart_arg, "colour");
        if (colour_cart && !sw_spec(swm)->colour) {
            fprintf(stderr, "rip: the %s takes no colour cartridge\n", model_arg);
            fclose(f);
            return 2;
        }
        h.sw_model = swm;
        h.colour = colour_cart;
        h.sw = out_sw ? fopen(out_sw, "wb") : NULL;
        if (out_sw && !h.sw) { perror(out_sw); fclose(f); return 1; }
        sw_page_cfg_t pc = { .model = swm, .colour_cart = colour_cart, .out = sw_out, .ctx = &h };
        if (!(swp = sw_page_new(&pc))) { fclose(f); return 1; }
        colour = colour_cart;
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
        .ink_row  = out_preview || (stylewriter && out_pbm) ? on_ink_row : NULL,
        .log      = say,
    };
    if (stylewriter) sw_rip_cfg(&cfg, &stage, swp, swm, algo, colour, !strcmp(tone_arg, "apple"));

    qdw_t qw;
    memset(&qw, 0, sizeof qw);
    qd_page_t *qdp = NULL;
    uint8_t *qd_keep = NULL;
    if (plw300) {
        static const qd_ops_t qops = { .put = qdw_put, .flush = qdw_flush, .read = qdw_read,
                                       .sleep_ms = qdw_sleep, .now_ms = qdw_now };
        qd_ops_t ops = qops;
        ops.ctx = &qw;
        qw.screen = algo;
        qw.frames = out_qd ? fopen(out_qd, "wb") : NULL;
        if (out_qd && !qw.frames) { perror(out_qd); fclose(f); return 1; }
        qd_job_cfg_t jc = { .model = QD_PLW300, .manual = !strcmp(source, "manual") };
        qd_keep = malloc(QD_KEEP_CAP);
        qd_page_cfg_t pc = { .model = QD_PLW300, .screen = algo, .gain = gain, .keep = qd_keep,
                             .keep_cap = qd_keep ? QD_KEEP_CAP : 0,
                             .sink = { qdw_open, qdw_row, qdw_print, qdw_note, &qw } };
        if (!(qw.sess = qd_session_new(&ops, &jc)) || !(qdp = qd_page_new(&pc)) ||
            qd_session_begin(qw.sess)) {
            fprintf(stderr, "rip: the 300's session could not start\n");
            fclose(f);
            return 1;
        }
        qd_rip_cfg(&cfg, &stage, qdp, &pc);
    }

    ripjob_stats_t st;
    memset(&st, 0, sizeof st);

    rc = ripjob_run(&u, &cfg, &st);
    if (plw300) {
        if (rc == 0 && qd_session_end(qw.sess)) rc = -1;
        if (qd_page_failed(qdp)) rc = -1;
        qd_page_free(qdp);
        qd_session_free(qw.sess);
        free(qd_keep);
        if (qw.frames && fclose(qw.frames)) qw.failed = 1;
        if (out_qd_bits) {
            FILE *bf = fopen(out_qd_bits, "wb");
            size_t nb = (qw.bits_pos + 7) / 8;
            if (qw.bits_pos & 7) qd_bits_marks(qw.bits, nb * 8, &qw.bits_pos, 8);   // end on marks
            if (!bf || fwrite(qw.bits, 1, nb, bf) != nb) qw.failed = 1;
            if (bf) fclose(bf);
        }
        free(qw.bits);
        if (qw.failed) { fprintf(stderr, "rip: could not write the 300's stream\n"); rc = -1; }
    }
    if (ps) {
        ps_job_end(ps);
        ps_free(ps);
        if (h.ps) fclose(h.ps);
    }
    if (h.pbm && write_pbm(out_pbm, h.pbm, h.dst_w, h.dst_h))
        fprintf(stderr, "rip: could not write %s\n", out_pbm);
    if (stylewriter) {
        sw_page_free(swp);
        if (h.swp[IW_BLACK] && h.dst_w && write_sw_pbms(out_pbm, &h, sw_spec(swm))) rc = -1;
        if (h.sw) fclose(h.sw);
    }

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
    for (int i = 0; i < IW_INKS; i++) free(h.swp[i]);
    urf_free(&u);
    fclose(f);

    if (rc < 0) return 1;
    fprintf(stderr, "rip: %d page%s\n", st.pages, st.pages == 1 ? "" : "s");
    return 0;
}
