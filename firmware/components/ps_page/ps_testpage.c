// SPDX-License-Identifier: GPL-3.0-or-later
// The PostScript printers' test page (W-1088); see ps_testpage.h.
#include "ps_testpage.h"
#include "halftone.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    void (*out)(void *ctx, const uint8_t *b, size_t n);
    void  *ctx;
} w_t;

static void say(w_t *w, const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) w->out(w->ctx, (const uint8_t *)line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

// A UTF-8 string as a PostScript string in StandardEncoding: printable ASCII
// as it is (its ' is quoteright, its ` quoteleft), the curly quotes onto
// those, anything else '?'; ( ) and \ escaped. Cut to fit out.
static const char *pstr(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p && k + 3 < cap) {
        unsigned char c = *p;
        if (c >= 0x80) {
            uint32_t cp = 0;
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            cp = *p++ & (0x3F >> more);
            for (int i = 0; i < more && (*p & 0xC0) == 0x80; i++) cp = cp << 6 | (*p++ & 0x3F);
            while ((*p & 0xC0) == 0x80) p++;
            c = cp == 0x2019 ? '\'' : cp == 0x2018 ? '`' : cp == 0x201C || cp == 0x201D ? '"' : '?';
        } else {
            p++;
            if (c < 0x20) continue;
        }
        if (c == '(' || c == ')' || c == '\\') out[k++] = '\\';
        out[k++] = (char)c;
    }
    out[k] = '\0';
    return out;
}

static void font(w_t *w, const char *name, int size)
{
    say(w, "/%s findfont %d scalefont setfont\n", name, size);
}

static void text(w_t *w, double x, double y, const char *s)
{
    char e[200];
    say(w, "%.1f %.1f moveto (%s) show\n", x, y, pstr(e, sizeof e, s));
}

// A label and its value on one line of the details block.
static void detail(w_t *w, double y, const char *label, const char *value)
{
    font(w, "Helvetica-Bold", 10);
    text(w, 54, y, label);
    font(w, "Helvetica", 10);
    text(w, 144, y, value);
}

// One screen's swatch: a ramp from white to black, W x H dots at 300 dpi,
// screened by AirWriter's own halftone and placed at x, y (points).
enum { SW_W = 390, SW_H = 150, SW_B = (SW_W + 7) / 8 };

static int swatch(w_t *w, ht_algo_t algo, double x, double y, const char *name)
{
    ht_t h;
    if (ht_init(&h, algo, SW_W)) return -1;
    uint8_t gray[SW_W], bits[SW_B];
    for (int i = 0; i < SW_W; i++) gray[i] = (uint8_t)(255 - i * 255 / (SW_W - 1));
    say(w, "gsave %.2f %.2f translate %.2f %.2f scale\n", x, y, SW_W * 72.0 / 300, SW_H * 72.0 / 300);
    say(w, "%d %d true [%d 0 0 -%d 0 %d] { currentfile aws readhexstring pop } imagemask\n",
        SW_W, SW_H, SW_W, SW_H, SW_H);
    static const char hex[] = "0123456789abcdef";
    for (int r = 0; r < SW_H; r++) {
        ht_row(&h, gray, bits);
        char line[2 * SW_B + 2];
        for (int b = 0; b < SW_B; b++) {
            line[2 * b] = hex[bits[b] >> 4];
            line[2 * b + 1] = hex[bits[b] & 15];
        }
        line[2 * SW_B] = '\n';
        w->out(w->ctx, (const uint8_t *)line, 2 * SW_B + 1);
    }
    say(w, "grestore\n");
    say(w, "0 setgray 0.25 setlinewidth %.2f %.2f %.2f %.2f awbox stroke\n", x, y,
        SW_W * 72.0 / 300, SW_H * 72.0 / 300);
    ht_free(&h);
    font(w, "Helvetica", 8);
    text(w, x, y - 11, name);
    return 0;
}

int ps_testpage(const ps_tp_info_t *i, void (*out)(void *ctx, const uint8_t *b, size_t n), void *ctx)
{
    w_t w = { out, ctx };
    say(&w, "%%!PS-Adobe-3.0\n%%%%Title: (AirWriter test page)\n%%%%Creator: AirWriter\n");
    say(&w, "%%%%LanguageLevel: 1\n%%%%Pages: 1\n");
    say(&w, "%%%%DocumentNeededResources: font Times-Roman Helvetica Helvetica-Bold\n");
    say(&w, "%%%%EndComments\n%%%%BeginProlog\n");
    // x y w h awbox: a closed rectangle path, Level 1 (no rectfill).
    say(&w, "/awbox { 4 2 roll moveto 1 index 0 rlineto 0 exch rlineto neg 0 rlineto closepath } bind def\n");
    say(&w, "/aws %d string def\n", SW_B);
    say(&w, "%%%%EndProlog\n%%%%Page: 1 1\nsave\n");

    // The mark and the printer's name.
    font(&w, "Times-Roman", 40);
    text(&w, 54, 724, "AirWriter");
    font(&w, "Helvetica", 11);
    text(&w, 56, 706, "Test page");
    char e[200];
    font(&w, "Times-Roman", 20);
    say(&w, "(%s) dup stringwidth pop 558 exch sub 724 moveto show\n",
        pstr(e, sizeof e, i->printer));
    say(&w, "0.5 setlinewidth 54 694 moveto 558 694 lineto stroke\n");

    // The details.
    char prod[160];
    if (i->product && i->product[0] && i->version && i->version[0])
        snprintf(prod, sizeof prod, "%s, PostScript version %s", i->product, i->version);
    else
        snprintf(prod, sizeof prod, "%s", i->product && i->product[0] ? i->product : "PostScript printer");
    detail(&w, 672, "Printer", i->printer);
    detail(&w, 658, "Product", prod);
    detail(&w, 644, "Shared by", i->board);
    detail(&w, 630, "Settings", i->host);

    // The printer's own type.
    font(&w, "Times-Roman", 24);
    text(&w, 54, 592, "Times: The quick brown fox jumps.");
    font(&w, "Times-Roman", 11);
    text(&w, 54, 576, "Times 11: Pack my box with five dozen liquor jugs. 0123456789");
    font(&w, "Helvetica", 24);
    text(&w, 54, 548, "Helvetica: Sphinx of black quartz.");
    font(&w, "Helvetica", 11);
    text(&w, 54, 532, "Helvetica 11: How vexingly quick daft zebras jump! 0123456789");

    // Gray, as the printer's own screen draws it: 0 to 100% ink.
    font(&w, "Helvetica-Bold", 10);
    text(&w, 54, 504, "The printer's gray");
    for (int k = 0; k <= 10; k++) {
        double x = 54 + k * 45.8;
        say(&w, "%.3f setgray %.1f 454 40 40 awbox fill\n", 1.0 - k / 10.0, x);
        say(&w, "0 setgray 0.25 setlinewidth %.1f 454 40 40 awbox stroke\n", x);
        font(&w, "Helvetica", 7);
        char pct[8];
        snprintf(pct, sizeof pct, "%d%%", k * 10);
        text(&w, x + 12, 444, pct);
    }

    // AirWriter's five screens, as it sends a photo: 300 dpi, one dot a bit.
    font(&w, "Helvetica-Bold", 10);
    text(&w, 54, 414, "AirWriter's screens, 300 dpi");
    static const struct { ht_algo_t a; const char *name; } sc[] = {
        { HT_ATKINSON, "Atkinson" }, { HT_FLOYD, "Floyd-Steinberg" }, { HT_BLUENOISE, "Blue noise" },
        { HT_BAYER, "Bayer" }, { HT_THRESHOLD, "Threshold" },
    };
    int rc = 0;
    for (int k = 0; k < 5 && !rc; k++) rc = swatch(&w, sc[k].a, 54 + k * 102.6, 362, sc[k].name);

    // A ruler, 7 in in eighths, and four one-inch squares.
    font(&w, "Helvetica-Bold", 10);
    text(&w, 54, 318, "A ruler and one-inch squares");
    say(&w, "0 setgray 0.5 setlinewidth 54 290 moveto 558 290 lineto stroke\n");
    for (int t = 0; t <= 56; t++) {
        double x = 54 + t * 9.0;
        int len = t % 8 == 0 ? 14 : t % 4 == 0 ? 9 : t % 2 == 0 ? 6 : 4;
        say(&w, "%.1f 290 moveto 0 %d rlineto stroke\n", x, len);
        if (t % 8 == 0) {
            font(&w, "Helvetica", 7);
            char n[4];
            snprintf(n, sizeof n, "%d", t / 8);
            text(&w, x + 2, 296, n);
        }
    }
    for (int k = 0; k < 4; k++) {
        double x = 54 + k * 144;
        say(&w, "0.5 setlinewidth %.1f 180 72 72 awbox stroke\n", x);
        font(&w, "Helvetica", 7);
        text(&w, x + 28, 212, "1 in");
    }
    font(&w, "Helvetica", 8);
    text(&w, 54, 150, "Printed by AirWriter. If the squares measure one inch, this printer prints true.");

    say(&w, "restore showpage\n%%%%Trailer\n%%%%EOF\n");
    return rc;
}
