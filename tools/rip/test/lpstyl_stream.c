// SPDX-License-Identifier: GPL-3.0-or-later
//
// A page as lpstyl would send it (W-1103), for swcheck.py: the dots in four PBMs
// (STEM-c.pbm, -m, -y, -k, as `rip --printer stylewriter --out-pbm` writes them) coded
// with lpstyl's coder (lpstyl_encode.c) into the wire stream swfake.py decode reads:
// rects, G blocks, 0C. Like lpstyl, a block gets its own rect and its first row is
// coded on its own, so the delta starts again with every block, where the page
// stage's carries across the blocks of a band. The two streams differ byte for
// byte and decode to the same dots, which is what swcheck holds them to.
//
//     lpstyl_stream MODEL CART STEM OUT.bin     MODEL: original ii 1200 1500 2200 2400 2500
//                                               CART: 0 black, 1 colour
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lpstyl_encode.h"
#include "sw_model.h"

typedef struct { unsigned w, h; size_t rb; uint8_t *bits; } pbm_t;

static int read_pbm(const char *stem, char ink, pbm_t *p)
{
    char path[1024];
    snprintf(path, sizeof path, "%s-%c.pbm", stem, ink);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fscanf(f, "P4 %u %u", &p->w, &p->h) != 2 || fgetc(f) != '\n') {
        fprintf(stderr, "lpstyl_stream: %s is not a raw PBM\n", path);
        fclose(f);
        return -1;
    }
    p->rb = (p->w + 7) / 8;
    p->bits = malloc(p->rb * p->h + 1);
    int bad = !p->bits || fread(p->bits, 1, p->rb * p->h, f) != p->rb * p->h;
    fclose(f);
    if (bad) fprintf(stderr, "lpstyl_stream: %s is short\n", path);
    return bad ? -1 : 0;
}

static void put_u16(FILE *f, unsigned v)
{
    fputc((int)(v & 255), f);
    fputc((int)(v >> 8), f);
}

int main(int argc, char **argv)
{
    static const struct { const char *name; sw_model_t m; } models[] = {
        { "original", SW_ORIGINAL }, { "ii", SW_II }, { "1200", SW_1200 }, { "1500", SW_C1500 },
        { "2200", SW_C2200 }, { "2400", SW_C2400 }, { "2500", SW_C2500 },
    };
    if (argc != 5) { fputs("usage: lpstyl_stream MODEL CART STEM OUT.bin\n", stderr); return 2; }
    sw_model_t m = SW_NONE;
    for (size_t i = 0; i < sizeof models / sizeof models[0]; i++)
        if (!strcmp(argv[1], models[i].name)) m = models[i].m;
    const sw_spec_t *spec = sw_spec(m);
    if (!spec) { fprintf(stderr, "lpstyl_stream: no model %s\n", argv[1]); return 2; }

    static const char inks[4] = { 'c', 'm', 'y', 'k' };
    pbm_t pl[4];
    memset(pl, 0, sizeof pl);
    for (int i = 0; i < 4; i++)
        if (read_pbm(argv[3], inks[i], &pl[i])) return 1;
    for (int i = 1; i < 4; i++)
        if (pl[i].w != pl[0].w || pl[i].h != pl[0].h) { fputs("lpstyl_stream: planes differ in size\n", stderr); return 1; }
    unsigned w = pl[0].w, h = pl[0].h;
    size_t rb = pl[0].rb;

    // lpstyl prints in colour when the file is and the printer can.
    int colour = atoi(argv[2]) && spec->colour;
    if (colour) {
        int any = 0;
        for (int i = 0; i < 3; i++)
            for (size_t b = 0; b < rb * h && !any; b++) any |= pl[i].bits[b] != 0;
        colour = any;
    }
    int planes = colour ? 4 : 1;
    uint32_t cap = sw_block_cap(m, planes);
    // The most a row of every plane codes to: lpstyl's coder spends a header byte on a
    // one-byte literal beside each one-byte white or black run, a half again.
    size_t room = (size_t)planes * (rb * 3 / 2 + 16);
    uint8_t *blk = malloc(cap + 16);

    FILE *out = fopen(argv[4], "wb");
    if (!out || !blk) { perror(argv[4]); return 1; }

    for (unsigned band = 0; band < h; band += spec->band_rows) {
        unsigned end = band + spec->band_rows < h ? band + spec->band_rows : h;
        for (unsigned r = band; r < end;) {
            unsigned first = r;
            size_t n = 0;
            while (r < end) {
                if (n && n + room > cap) break;
                for (int k = 0; k < planes; k++) {
                    const pbm_t *p = &pl[colour ? k : 3];
                    const uint8_t *row = p->bits + (size_t)r * rb;
                    const uint8_t *prev = spec->delta && r > first ? row - rb : NULL;
                    n += lpstyl_appendEncode(rb, row, prev, blk + n);
                }
                r++;
            }
            fputc(colour ? 'c' : 'R', out);
            put_u16(out, spec->rect_dx);
            put_u16(out, first);
            put_u16(out, spec->rect_dx + w - 1);
            put_u16(out, r - 1);
            fputc('G', out);
            put_u16(out, (unsigned)n);
            fwrite(blk, 1, n, out);
            fputc(0, out);
        }
    }
    fputc(0x0C, out);
    if (fclose(out)) { perror(argv[4]); return 1; }
    return 0;
}
