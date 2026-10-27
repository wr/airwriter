// SPDX-License-Identifier: GPL-3.0-or-later
//
// The test page, top to bottom as the design lays it out. Text is the
// printer's own: escape codes and characters, a CR and a line feed, placed by
// spaces in a fixed pitch. Pictures are bands of bit-image strips, 8 rows
// each, filled a row at a time by the band's own function and sent as a
// colour AirPrint page's are (iwc): one pass per ink from the left margin to
// its last dot. Never ESC F: on board 1's first print, lines near it landed
// inches to the right and a character placed by it wrapped to the margin.
// Text never shares a line with graphics, so NLQ's two passes never meet a
// bit image, and NLQ never prints in a line of several CR-only passes.
#include "testpage.h"
#include "tp_scene.h"
#include "tp_art.h"
#include "ink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W   1152                    // dots across, 144 dpi: 8 in
#define NB  (W / 8)
#define ESC "\x1b"

static const char k_ink_digit[IW_INKS] = { '1', '2', '3', '0' };

typedef struct {
    const tp_io_t   *io;
    const tp_info_t *info;
    int      colour;
    int      lf;                    // the line feed set, 1/144 in; 0 for none yet
    int      ink;                   // the ink selected, iw_ink_t
    int      stopped, nomem;
    uint8_t (*rows)[8][NB];         // a strip, per ink
    uint8_t *cols;
} tp_t;

static void send(tp_t *t, const void *b, size_t n) { t->io->out(t->io->out_ctx, b, n); }
static void sends(tp_t *t, const char *s)          { send(t, s, strlen(s)); }

static int halted(tp_t *t)
{
    if (!t->stopped && !t->nomem && t->io->stop && t->io->stop(t->io->stop_ctx)) t->stopped = 1;
    return t->stopped || t->nomem;
}

// Moves the paper n/144 in, in line feeds of at most 99/144.
static void feed(tp_t *t, int n)
{
    while (!t->stopped && !t->nomem && n > 0) {
        int k = n > 99 ? 99 : n;
        if (t->lf != k) {
            char s[6];
            snprintf(s, sizeof s, ESC "T%02d", k);
            sends(t, s);
            t->lf = k;
        }
        sends(t, "\n");
        n -= k;
    }
}

static void set_ink(tp_t *t, int ink)
{
    if (t->ink == ink) return;
    const char s[3] = { 0x1B, 'K', k_ink_digit[ink] };
    send(t, s, 3);
    t->ink = ink;
}

// ------------------------------------------------------------------- text

typedef struct { char s[640]; size_t n; } ln_t;

static void add(ln_t *l, const char *s)
{
    size_t k = strlen(s);
    if (l->n + k > sizeof l->s) k = sizeof l->s - l->n;
    memcpy(l->s + l->n, s, k);
    l->n += k;
}

static void addc(ln_t *l, char c) { if (l->n < sizeof l->s) l->s[l->n++] = c; }

// A value from outside, as the printer can print it in at most cap
// characters: printable ASCII kept, anything else one '?' a character
// (UTF-8's continuation bytes dropped), padded with spaces to pad.
static void addv(ln_t *l, const char *v, size_t cap, size_t pad)
{
    size_t k = 0;
    for (; v && *v && k < cap; v++) {
        unsigned char c = (unsigned char)*v;
        if (c >= 0x80 && c < 0xC0) continue;
        addc(l, c >= 0x20 && c < 0x7F ? (char)c : '?');
        k++;
    }
    for (; k < pad; k++) addc(l, ' ');
}

// A line of text, then the paper on by lead/144 in.
static void line(tp_t *t, const ln_t *l, int lead)
{
    if (halted(t)) return;
    send(t, l->s, l->n);
    sends(t, "\r");
    feed(t, lead);
}

static void text(tp_t *t, const char *s, int lead)
{
    ln_t l = { .n = 0 };
    add(&l, s);
    line(t, &l, lead);
}

// A one-dot rule across the page: ESC V repeats a dot column. Pin 5, four
// rows (8/144 in) below the line: board 1's top pin is dead, and a rule on it
// does not print at all.
#define RULE_DROP 8

static void rule(tp_t *t, int lead)
{
    if (halted(t)) return;
    static const char r[] = ESC "p" ESC "V1152\x10\r";
    send(t, r, sizeof r - 1);
    feed(t, lead);
}

static void heading(tp_t *t, const char *s)
{
    ln_t l = { .n = 0 };
    add(&l, ESC "a2" ESC "N" ESC "!");
    add(&l, s);
    add(&l, ESC "\"");
    line(t, &l, 18 - RULE_DROP);
    rule(t, 14 + RULE_DROP);
}

// --------------------------------------------------------------- graphics

static void dot(uint8_t *row, uint32_t x)     { row[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); }

// w bits of src into row from x0, both MSB first.
static void blit(uint8_t *row, uint32_t x0, const uint8_t *src, uint32_t w)
{
    for (uint32_t x = 0; x < w; x++)
        if (src[x >> 3] & (0x80 >> (x & 7))) dot(row, x0 + x);
}

// Fills row y of a band: one 1bpp row per ink, cleared. 0, or -1 when out
// of memory.
typedef int (*fill_fn)(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS]);

static void band(tp_t *t, uint32_t rows, fill_fn fill, void *ctx)
{
    if (halted(t)) return;
    sends(t, ESC "p");
    for (uint32_t s = 0; s < rows; s += 8) {
        if (halted(t)) return;
        memset(t->rows, 0, sizeof(uint8_t[IW_INKS][8][NB]));
        for (uint32_t r = 0; r < 8 && s + r < rows; r++) {
            uint8_t *const bits[IW_INKS] = { t->rows[0][r], t->rows[1][r], t->rows[2][r],
                                             t->rows[3][r] };
            if (fill(ctx, s + r, bits)) { t->nomem = 1; return; }
        }
        for (int ink = 0; ink < IW_INKS; ink++) {
            const uint8_t *rp[8];
            for (int r = 0; r < 8; r++) rp[r] = t->rows[ink][r];
            iw_transpose(rp, t->cols, W);
            uint32_t z = W;
            while (z && !t->cols[z - 1]) z--;
            if (!z) continue;
            if (t->colour) set_ink(t, ink);
            char h[8];
            snprintf(h, sizeof h, ESC "G%04u", (unsigned)z);
            sends(t, h);
            send(t, t->cols, z);
            sends(t, "\r");
        }
        feed(t, 16);
    }
}

// Halftoners for a band, made and freed with it.
static int ht_make(ht_t *h, ht_algo_t algo, int gain, uint32_t w, int ink)
{
    if (ht_init(h, algo, w)) return -1;
    ht_set_gain(h, gain);
    ink_screen_phase(h, (iw_ink_t)ink);
    return 0;
}

// ---------------------------------------------------------------- header

typedef struct {
    int colour;
    ink_screens_t inks;             // the icon, colour ribbon
    ht_t gray;                      // the icon, black ribbon
    uint8_t rgb[TP_ICON_W * 4 * 3], g[TP_ICON_W * 4], ib[IW_INKS][TP_ICON_W * 4 / 8];
} header_t;

static int fill_header(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    header_t *h = ctx;
    const uint32_t iw = TP_ICON_W * 4;          // each icon pixel 4 dots by 2 rows
    for (uint32_t x = 0; x < iw; x++)
        memcpy(h->rgb + x * 3, tp_icon_rgb[tp_icon[y / 2][x / 4]], 3);
    if (h->colour) {
        uint8_t *const ib[IW_INKS] = { h->ib[0], h->ib[1], h->ib[2], h->ib[3] };
        ink_screens_row(&h->inks, h->rgb, ib);
        for (int i = 0; i < IW_INKS; i++) memcpy(bits[i], h->ib[i], iw / 8);
    } else {
        ink_gray(h->rgb, iw, h->g);
        ht_row(&h->gray, h->g, bits[IW_BLACK]);
    }
    if (y >= 14 && y < 14 + TP_TITLE_H) blit(bits[IW_BLACK], 168, tp_title[y - 14], TP_TITLE_W);
    if (y >= 28 && y < 28 + TP_SUB_H) blit(bits[IW_BLACK], W - TP_SUB_W, tp_sub[y - 28], TP_SUB_W);
    return 0;
}

static void header(tp_t *t)
{
    header_t *h = calloc(1, sizeof *h);
    if (h) h->colour = t->colour;
    if (!h || (t->colour ? ink_screens_init(&h->inks, HT_BAYER, 0, TP_ICON_W * 4)
                         : ht_make(&h->gray, HT_BAYER, 0, TP_ICON_W * 4, IW_BLACK))) {
        free(h);
        t->nomem = 1;
        return;
    }
    band(t, 56, fill_header, h);
    if (t->colour) ink_screens_free(&h->inks); else ht_free(&h->gray);
    free(h);
}

// -------------------------------------------------------------- identity

static const char *screen_name(ht_algo_t a)
{
    switch (a) {
    case HT_ATKINSON:  return "Atkinson";
    case HT_FLOYD:     return "Floyd-Steinberg";
    case HT_BLUENOISE: return "Blue noise";
    case HT_BAYER:     return "Ordered";
    case HT_APPLE:     return "Apple";
    default:           return "Threshold";
    }
}

// Labels 11 characters; the left value at most 34 and the right column at
// character 47, its value at most 38: 96 characters at 12 cpi, 8 in.
static void kv(tp_t *t, const char *k1, const char *v1, const char *k2, const char *v2, int first)
{
    ln_t l = { .n = 0 };
    if (first) add(&l, ESC "a0" ESC "E");
    addv(&l, k1, 11, 11);
    addv(&l, v1, 34, 36);
    addv(&l, k2, 11, 11);
    addv(&l, v2, 38, 0);
    line(t, &l, 20);
}

static void identity(tp_t *t)
{
    const tp_info_t *i = t->info;
    char printer[80], addr[80], settings[80], firmware[64];
    snprintf(printer, sizeof printer, "%s, %s", i->model,
             i->ribbon == IW_RIBBON_COLOUR ? "colour ribbon"
             : i->ribbon == IW_RIBBON_BLACK ? "black ribbon" : "ribbon not known");
    if (i->setup_ssid) snprintf(addr, sizeof addr, "join %s", i->setup_ssid);
    else snprintf(addr, sizeof addr, "%s.local  %s", i->host,
                  i->ip && i->ip[0] && strcmp(i->ip, "0.0.0.0") ? i->ip : "no IP yet");
    if (i->setup_ssid) snprintf(settings, sizeof settings, "http://%s", i->ip);
    else snprintf(settings, sizeof settings, "http://%s.local", i->host);
    snprintf(firmware, sizeof firmware, "AirWriter %s", i->version);
    kv(t, "Printer", printer, "Firmware", firmware, 1);
    kv(t, "Name", i->name, "Built", i->built, 0);
    kv(t, i->setup_ssid ? "Wi-Fi" : "Address", addr, "Halftone", screen_name(i->screen), 0);
    // The ways into AppleTalk can be more than the right column holds: the
    // rest goes on a fifth row under it, broken after a comma.
    const char *at = i->appletalk ? i->appletalk : "";
    char at1[48], at2[96] = "";
    snprintf(at1, sizeof at1, "%s", at);
    if (strlen(at) > 38) {
        const char *cut = NULL;
        for (const char *c = at; c[0] && c[1] && c - at < 38; c++)
            if (c[0] == ',' && c[1] == ' ') cut = c;
        if (cut) {
            snprintf(at1, sizeof at1, "%.*s", (int)(cut - at + 1), at);
            snprintf(at2, sizeof at2, "%s", cut + 2);
        }
    }
    kv(t, "Settings", settings, "AppleTalk", at1, 0);
    if (at2[0]) kv(t, "", "", "", at2, 0);
}

// ------------------------------------------------------------------ text

#define FOX "The quick brown fox jumps over the lazy dog 0123456789"

static void label(ln_t *l, const char *s)
{
    add(l, ESC "a0" ESC "N");
    addv(l, s, 20, 20);
}

static void text_section(tp_t *t)
{
    heading(t, "Text");
    static const struct { const char *name, *cmd; } q[] = {
        { "Draft", ESC "a1" }, { "Correspondence", ESC "a0" }, { "Near letter quality", ESC "a2" },
        { "Proportional", ESC "a2" ESC "p" },
    };
    for (size_t k = 0; k < sizeof q / sizeof q[0]; k++) {
        ln_t l = { .n = 0 };
        label(&l, q[k].name);
        add(&l, q[k].cmd);
        add(&l, FOX);
        line(t, &l, 22);
    }
    static const struct { const char *name, *cmd; } p[] = {
        { "9 cpi", ESC "n" }, { "10 cpi", ESC "N" }, { "12 cpi", ESC "E" },
        { "13.4 cpi", ESC "e" }, { "15 cpi", ESC "q" }, { "17 cpi", ESC "Q" },
    };
    for (size_t k = 0; k < sizeof p / sizeof p[0]; k++) {
        ln_t l = { .n = 0 };
        label(&l, p[k].name);
        add(&l, p[k].cmd);
        add(&l, FOX);
        line(t, &l, 20);
    }
    ln_t l = { .n = 0 };
    label(&l, "Characters");
    add(&l, ESC "Q");
    for (char c = 0x21; c < 0x7F; c++) addc(&l, c);
    line(t, &l, 22);

    l.n = 0;
    label(&l, "Styles");
    add(&l, ESC "!" "Bold" ESC "\"" "  " ESC "X" "Underline" ESC "Y" "  \x0e" "Wide" "\x0f" "  "
            ESC "w" "Half height" ESC "W" "  E=mc" ESC "x" "2" ESC "z" "  H" ESC "y" "2" ESC "z" "O");
    line(t, &l, 22);

    l.n = 0;
    label(&l, "MouseText");
    add(&l, ESC "&");
    for (char c = 0x40; c < 0x60; c++) addc(&l, c);
    add(&l, ESC "$");
    line(t, &l, 22);
}

// ---------------------------------------------------------------- colour

#define PH     72               // the still life's rows in the colour section, 1 in
#define PW     432              // and its dots across, 3 in
#define RX     504              // where the ramps and steps start
#define RW     (W - RX)

static int fill_swatches(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)ctx; (void)y;
    // Yellow, magenta, cyan, black, then the printer's own overprints:
    // orange, green and purple.
    static const uint8_t mix[7] = {
        1 << IW_YELLOW, 1 << IW_MAGENTA, 1 << IW_CYAN, 1 << IW_BLACK,
        1 << IW_YELLOW | 1 << IW_MAGENTA, 1 << IW_YELLOW | 1 << IW_CYAN,
        1 << IW_MAGENTA | 1 << IW_CYAN,
    };
    for (int i = 0; i < 7; i++)
        for (int ink = 0; ink < IW_INKS; ink++)
            if (mix[i] >> ink & 1)
                memset(bits[ink] + i * 168 / 8, 0xFF, 144 / 8);    // 168: 21 bytes
    return 0;
}

typedef struct {
    const tp_info_t *info;
    ink_screens_t inks;
    ht_t ramp;
    int ramp_ink;               // the ramp's ink, -1 before the first
    uint8_t rgb[PW * 3], g[RW], b[RW / 8], ib[IW_INKS][PW / 8];
} photo_t;

// The still life in four inks, beside a ramp of each ink, cyan, magenta,
// yellow and black, 16 rows each on 18.
static int fill_colour_photo(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    photo_t *p = ctx;
    tp_scene_row(PW, PH, y, p->rgb);
    uint8_t *const ib[IW_INKS] = { p->ib[0], p->ib[1], p->ib[2], p->ib[3] };
    ink_screens_row(&p->inks, p->rgb, ib);
    for (int i = 0; i < IW_INKS; i++) memcpy(bits[i], p->ib[i], PW / 8);

    static const int order[4] = { IW_CYAN, IW_MAGENTA, IW_YELLOW, IW_BLACK };
    uint32_t j = y / 18;
    if (j >= 4 || y % 18 >= 16) return 0;
    if (p->ramp_ink != order[j]) {
        if (p->ramp_ink >= 0) ht_free(&p->ramp);
        p->ramp_ink = order[j];
        if (ht_make(&p->ramp, p->info->screen, p->info->gain, RW, order[j])) {
            p->ramp_ink = -1;
            return -1;
        }
    }
    for (uint32_t x = 0; x < RW; x++) p->g[x] = (uint8_t)(255.0 - 255.0 * x / (RW - 1));
    ht_row(&p->ramp, p->g, p->b);
    memcpy(bits[order[j]] + RX / 8, p->b, RW / 8);
    return 0;
}

// The black ribbon's: the still life in gray beside eleven steps, 0-100 %.
#define STEP_W 59
static const uint8_t step_gray[11] = { 255, 229, 204, 179, 153, 127, 102, 77, 51, 25, 0 };

typedef struct {
    const tp_info_t *info;
    ht_t photo, step[11];
    uint8_t rgb[PW * 3], g[PW], b[PW / 8];
} gray_photo_t;

static int fill_gray_photo(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    gray_photo_t *p = ctx;
    tp_scene_row(PW, PH, y, p->rgb);
    ink_gray(p->rgb, PW, p->g);
    ht_row(&p->photo, p->g, bits[IW_BLACK]);
    for (int j = 0; j < 11; j++) {
        memset(p->g, step_gray[j], STEP_W - 6);
        ht_row(&p->step[j], p->g, p->b);
        blit(bits[IW_BLACK], RX + j * STEP_W, p->b, STEP_W - 6);
    }
    return 0;
}

static void colour_section(tp_t *t)
{
    const tp_info_t *i = t->info;
    if (t->colour) {
        heading(t, "Colour");
        // Each name in its own ink, one pass per ink, yellow's first.
        static const struct { char d; const char *name; int slot; } w[7] = {
            { '1', "Yellow", 0 }, { '4', "Orange", 4 }, { '5', "Green", 5 },
            { '2', "Magenta", 1 }, { '3', "Cyan", 2 }, { '6', "Purple", 6 }, { '0', "Black", 3 },
        };
        // In correspondence, one pass a word: NLQ's own two passes in a
        // line of CR-only passes is more than this page needs to ask. At
        // 12 cpi a swatch's 168 dots are 14 characters.
        if (!halted(t)) {
            for (int k = 0; k < 7; k++) {
                ln_t l = { .n = 0 };
                add(&l, ESC "K");
                addc(&l, w[k].d);
                add(&l, ESC "a0" ESC "E");
                addv(&l, "", 0, (size_t)w[k].slot * 14);
                add(&l, w[k].name);
                addc(&l, '\r');
                send(t, l.s, l.n);
            }
            sends(t, ESC "K0");
            t->ink = IW_BLACK;
            feed(t, 22);
        }
        band(t, 24, fill_swatches, NULL);
        feed(t, 12);

        photo_t *p = calloc(1, sizeof *p);
        if (!p || ink_screens_init(&p->inks, i->screen, i->gain, PW)) {
            free(p);
            t->nomem = 1;
            return;
        }
        p->info = i;
        p->ramp_ink = -1;
        band(t, PH, fill_colour_photo, p);
        if (p->ramp_ink >= 0) ht_free(&p->ramp);
        ink_screens_free(&p->inks);
        free(p);
        return;
    }

    heading(t, "Photo");
    text(t, i->ribbon == IW_RIBBON_BLACK
             ? ESC "a0" ESC "E" "The black ribbon is in. With the colour ribbon in, this part "
               "prints in colour."
             : ESC "a0" ESC "E" "AirWriter could not tell which ribbon is in, so this prints in "
               "black.", 26);
    gray_photo_t *p = calloc(1, sizeof *p);
    int made = 0;
    if (p) {
        p->info = i;
        made = !ht_make(&p->photo, i->screen, i->gain, PW, IW_BLACK);
        for (int j = 0; made && j < 11; j++)
            if (ht_make(&p->step[j], i->screen, i->gain, STEP_W - 6, IW_BLACK)) {
                while (j--) ht_free(&p->step[j]);
                ht_free(&p->photo);
                made = 0;
            }
    }
    if (!made) {
        free(p);
        t->nomem = 1;
        return;
    }
    band(t, PH, fill_gray_photo, p);
    for (int j = 0; j < 11; j++) ht_free(&p->step[j]);
    ht_free(&p->photo);
    free(p);

    feed(t, 4);
    // At 17 cpi a step's 59 dots are 7 characters, near enough; column 60 is
    // dot 508, 4 into the first step.
    ln_t l = { .n = 0 };
    add(&l, ESC "a0" ESC "Q");
    addv(&l, "", 0, 60);
    for (int j = 0; j < 11; j++) {
        char pc[8];
        snprintf(pc, sizeof pc, "%d%%", j * 10);
        addv(&l, pc, 7, j < 10 ? 7 : 0);
    }
    line(t, &l, 20);
}

// -------------------------------------------------------------- halftone

#define HW  216                     // a panel, 1.5 in
#define HG  234                     // panel to panel
#define HH  56                      // the still life's rows

static const ht_algo_t screens[5] = { HT_ATKINSON, HT_FLOYD, HT_BLUENOISE, HT_BAYER, HT_THRESHOLD };

typedef struct {
    const tp_info_t *info;
    ht_t ht[5];
    int  made;                      // 0 none, 1 for the pictures, 2 for the ramps
    uint8_t rgb[HW * 3], g[HW], b[HW / 8];
} panels_t;

static int panels_make(panels_t *p, int which)
{
    for (int i = 0; i < 5; i++) {
        if (p->made) ht_free(&p->ht[i]);
    }
    p->made = 0;
    for (int i = 0; i < 5; i++)
        if (ht_make(&p->ht[i], screens[i], p->info->gain, HW, IW_BLACK)) {
            while (i--) ht_free(&p->ht[i]);
            return -1;
        }
    p->made = which;
    return 0;
}

// The still life through each screen, then 4 rows down a 12-row ramp.
static int fill_panels(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    panels_t *p = ctx;
    if (y < HH) {
        if (p->made != 1 && panels_make(p, 1)) return -1;
        tp_scene_row(HW, HH, y, p->rgb);
        ink_gray(p->rgb, HW, p->g);
    } else if (y >= HH + 4) {
        if (p->made != 2 && panels_make(p, 2)) return -1;
        for (uint32_t x = 0; x < HW; x++) p->g[x] = (uint8_t)(255.0 - 255.0 * x / (HW - 1));
    } else {
        return 0;
    }
    for (int i = 0; i < 5; i++) {
        ht_row(&p->ht[i], p->g, p->b);
        blit(bits[IW_BLACK], (uint32_t)i * HG, p->b, HW);
    }
    return 0;
}

static void halftone_section(tp_t *t)
{
    const tp_info_t *i = t->info;
    heading(t, "Halftone");
    // At 12 cpi a character is 12 dots: the labels start at columns 0, 20,
    // 39, 59 and 78, within 6 dots of each panel's left edge.
    static const int col[5] = { 0, 20, 39, 59, 78 };
    ln_t l = { .n = 0 };
    add(&l, ESC "a0" ESC "E");
    int at = 0;
    for (int k = 0; k < 5; k++) {
        addv(&l, "", 0, (size_t)(col[k] - at));
        const char *name = screen_name(screens[k]);
        if (screens[k] == i->screen) {
            add(&l, ESC "!");
            add(&l, name);
            add(&l, ESC "\" " ESC "&D" ESC "$");       // MouseText's check mark
            at = col[k] + (int)strlen(name) + 2;
        } else {
            add(&l, name);
            at = col[k] + (int)strlen(name);
        }
    }
    line(t, &l, 22);

    panels_t *p = calloc(1, sizeof *p);
    if (!p) { t->nomem = 1; return; }
    p->info = i;
    band(t, HH + 16, fill_panels, p);
    if (p->made) for (int k = 0; k < 5; k++) ht_free(&p->ht[k]);
    free(p);
    feed(t, 2);
    text(t, ESC "a0" ESC "E" "The ticked screen prints your pages. Change it on the Printer page "
            "of the settings.", 22);
}

// ------------------------------------------------------------- alignment

static int fill_ruler(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)ctx;
    uint8_t *k = bits[IW_BLACK];
    if (y < 2) memset(k, 0xFF, NB);                     // two rows: one dead pin leaves one
    for (uint32_t x = 0; x < W; x += 9) {               // 1/16 in
        uint32_t h = x % 144 == 0 ? 8 : x % 72 == 0 ? 6 : x % 36 == 0 ? 4 : x % 18 == 0 ? 3 : 2;
        if (y < h) dot(k, x);
    }
    dot(k, W - 1);
    return 0;
}

// Four one-inch squares, at 0, 2, 4 and 6 in.
static int fill_squares(void *ctx, uint32_t y, uint8_t *const bits[IW_INKS])
{
    (void)ctx;
    uint8_t *k = bits[IW_BLACK];
    if (y % 18 == 0 || y == 71)                         // a quarter-inch grid
        for (uint32_t x = 0; x < 144; x++) dot(k, x);
    for (uint32_t x = 0; x <= 144; x += 36) dot(k, x < 144 ? x : 143);
    memset(k + 288 / 8, 0xFF, 144 / 8);                 // solid black
    for (uint32_t x = 576; x < 720; x += 4) dot(k, x);  // vertical lines
    for (uint32_t x = 864; x < 1008; x++)               // the finest checker
        if ((x - 864 + y) % 2 == 0) dot(k, x);
    return 0;
}

static void alignment_section(tp_t *t)
{
    heading(t, "Alignment");
    band(t, 8, fill_ruler, NULL);
    // At 10 cpi an inch is 10 characters: each number starts on its tick,
    // and the 8, which would start past the line, ends it.
    ln_t l = { .n = 0 };
    add(&l, ESC "a0" ESC "N");
    for (int i = 0; i < 8; i++) {
        char d[2] = { (char)('0' + i), '\0' };
        addv(&l, d, 1, 10);
    }
    l.n--;                                      // the 8 in the 80th column
    addc(&l, '8');
    line(t, &l, 24);
    band(t, 72, fill_squares, NULL);
    feed(t, 6);
    static const char *const caps[2][4] = {
        { "One inch each way", "Solid black", "Straight lines", "Even gray" },
        { "Measure it with a ruler", "A white line: a pin is out", "The head is aligned",
          "The paper feeds evenly" },
    };
    for (int r = 0; r < 2; r++) {                // 15 cpi: 2 in is 30 characters
        l.n = 0;
        add(&l, ESC "a0" ESC "q");
        for (int i = 0; i < 4; i++) addv(&l, caps[r][i], 30, i < 3 ? 30 : 0);
        line(t, &l, 18);
    }
}

// ------------------------------------------------------------------ page

tp_result_t tp_print(const tp_info_t *info, const tp_io_t *io)
{
    tp_t t = { .io = io, .info = info, .ink = IW_BLACK,
               .colour = info->ribbon == IW_RIBBON_COLOUR };
    t.rows = malloc(sizeof(uint8_t[IW_INKS][8][NB]));
    t.cols = malloc(W);
    if (!t.rows || !t.cols) {
        free(t.rows);
        free(t.cols);
        return TP_NOMEM;
    }
    enum { GAP = 10 };

    // Laid out as an AirPrint page is, from the sheet's top edge: its content
    // from 1 in down, where printing starts on paper loaded as Apple says
    // (under the paper bail). The top margin setting moves it as it moves an
    // AirPrint page, but drops no more than that blank inch: text cannot be
    // cropped.
    sends(&t, ESC "c");
    int top = 144 + info->top_margin;
    feed(&t, top > 0 ? top : 0);
    header(&t);
    rule(&t, 22);                               // its dot 8/144 down, as before
    identity(&t);
    feed(&t, GAP + 2);
    text_section(&t);
    feed(&t, GAP);
    colour_section(&t);
    feed(&t, GAP + 4);
    halftone_section(&t);
    feed(&t, GAP);
    alignment_section(&t);
    feed(&t, 12 - RULE_DROP);
    rule(&t, 10 + RULE_DROP);
    text(&t, ESC "a0" ESC "E" "AirWriter by Wells Workshop    wells.ee    github.com/wr/airwriter", 0);

    tp_result_t rc = t.nomem ? TP_NOMEM : t.stopped ? TP_STOPPED : TP_DONE;
    if (rc == TP_DONE) {
        if (t.colour) set_ink(&t, IW_BLACK);
        sends(&t, "\x0c");
    }
    free(t.rows);
    free(t.cols);
    return rc;
}
