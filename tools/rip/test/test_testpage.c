// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_util.h"
#include "tp_scene.h"
#include "testpage.h"
#include "imagewriter.h"

#include <stdlib.h>

/* The still life, 432 x 88 as the colour section draws it. */
enum { SW = 432, SH = 88 };

static const uint8_t *px(const uint8_t *img, int x, int y) { return img + ((size_t)y * SW + x) * 3; }

static uint8_t *scene(void)
{
    uint8_t *img = malloc((size_t)SW * SH * 3);
    for (int y = 0; y < SH; y++) tp_scene_row(SW, SH, (uint32_t)y, img + (size_t)y * SW * 3);
    return img;
}

static void test_scene(void)
{
    uint8_t *img = scene();

    /* the top row is sky: blue the most */
    for (int x = 0; x < SW; x += 37) {
        const uint8_t *p = px(img, x, 0);
        CHECK(p[2] > p[0] + 20 && p[2] > p[1]);
    }
    /* each ball, at its centre, in its own colour */
    const uint8_t *r = px(img, 131, 35), *g = px(img, 243, 46), *b = px(img, 290, 44),
                  *y = px(img, 191, 66);
    CHECK(r[0] > r[1] + 60 && r[0] > r[2] + 60);
    CHECK(g[1] > g[0] + 60 && g[1] > g[2] + 30);
    CHECK(b[2] > b[0] + 60 && b[2] > b[1] + 30);
    CHECK(y[0] > y[2] + 80 && y[1] > y[2] + 80);
    /* the floor along the bottom is a warm gray */
    for (int x = 0; x < SW; x += 37) {
        const uint8_t *p = px(img, x, SH - 1);
        CHECK(abs(p[0] - p[1]) < 15 && abs(p[1] - p[2]) < 15 && p[0] >= p[2]);
    }
    /* and the same every time */
    uint8_t *again = scene();
    CHECK_EQ_INT(memcmp(img, again, (size_t)SW * SH * 3), 0);
    free(again);
    free(img);
}

/* ------------------------------------------------------------------ page */

typedef struct { uint8_t *b; size_t n, cap; } buf_t;

static void put(void *ctx, const uint8_t *b, size_t n)
{
    buf_t *s = (buf_t *)ctx;
    if (s->n + n > s->cap) {
        s->cap = (s->n + n) * 2;
        s->b = realloc(s->b, s->cap);
    }
    memcpy(s->b + s->n, b, n);
    s->n += n;
}

static tp_info_t info_for(iw_ribbon_t ribbon)
{
    tp_info_t i = {
        .version = "v0.2.0-29-g4a93f60", .built = "Oct 20 2026", .model = "ImageWriter II",
        .name = "ImageWriter II", .host = "imagewriter-2434", .ip = "10.0.1.115",
        .appletalk = "AppleTalk port", .ribbon = ribbon, .screen = HT_ATKINSON, .gain = 0,
    };
    return i;
}

static buf_t print_page(const tp_info_t *info, tp_result_t *rc)
{
    buf_t s = { 0 };
    tp_io_t io = { .out = put, .out_ctx = &s };
    *rc = tp_print(info, &io);
    return s;
}

static int has(const buf_t *s, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= s->n; i++)
        if (!memcmp(s->b + i, needle, k)) return 1;
    return 0;
}

static int count(const buf_t *s, const char *needle, size_t k)
{
    int c = 0;
    for (size_t i = 0; i + k <= s->n; i++) c += !memcmp(s->b + i, needle, k);
    return c;
}

/* The printer, as far as the page drives it: where each line reaches across
 * (fixed pitch text and graphics; proportional widths are the printer's own
 * and unknown here), how far down anything prints, the inks used. Units: dots
 * at 144 dpi across, 1/144 in down. */
typedef struct {
    int    bad;                 // a command this reader does not know
    double max_x;               // the furthest right any fixed text or graphics reaches
    int    f_order_bad;         // an ESC F not after the last on its line, or past 1151
    int    max_y;               // the lowest anything prints, its bottom edge
    int    k_used[7], last_k, k_any;
    int    f_used;              // ESC F anywhere (board 1 misplaced lines near it)
    int    nlq_overprint;       // NLQ text in a line printed in more than one pass
    int    rule_top_pin_only;   // an ESC V rule on the top pin alone (a dead pin hides it)
    int    first_y;             // where anything first prints, -1 for nothing
} page_t;

static int num(const uint8_t *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++) v = v * 10 + (b[i] == ' ' ? 0 : b[i] - '0');
    return v;
}

static page_t read_page(const buf_t *s)
{
    page_t p = { .last_k = 0, .first_y = -1 };
    double dpi = 80, cpi = 10;          // ESC c leaves 10 cpi on these DIP switches
    int prop = 0, dw = 0, lf = 24, y = 0, last_f = -1, x_known = 1;
    int nlq = 0, passes = 0, printed = 0, line_nlq = 0;
    double x = 0;
    const uint8_t *b = s->b;
    for (size_t i = 0; i < s->n;) {
        uint8_t c = b[i++];
        if (c == 0x1B) {
            uint8_t k = b[i++];
            switch (k) {
            case 'c': dpi = 80; cpi = 10; prop = 0; dw = 0; lf = 24; nlq = 0; break;
            case 'n': dpi = 72;  cpi = 9;    prop = 0; break;
            case 'N': dpi = 80;  cpi = 10;   prop = 0; break;
            case 'E': dpi = 96;  cpi = 12;   prop = 0; break;
            case 'e': dpi = 107.2; cpi = 13.4; prop = 0; break;
            case 'q': dpi = 120; cpi = 15;   prop = 0; break;
            case 'Q': dpi = 136; cpi = 17;   prop = 0; break;
            case 'p': dpi = 144; prop = 1; break;
            case 'P': dpi = 160; prop = 1; break;
            case '!': case '"': case 'X': case 'Y': case 'w': case 'W': case 'x': case 'y':
            case 'z': case '&': case '$': case 'A': case 'B': case 'm': case 'M':
                if (k == 'A') lf = 24;
                if (k == 'B') lf = 18;
                break;
            case 'a': nlq = b[i++] == '2'; break;
            case 's': i++; break;
            case 'K': {
                int d = b[i++] - '0';
                if (d >= 0 && d < 7) p.k_used[d] = 1; else p.bad = 1;
                p.last_k = d; p.k_any = 1;
                break;
            }
            case 'T': lf = num(b + i, 2); i += 2; break;
            case 'F': {
                int f = num(b + i, 4); i += 4;
                p.f_used = 1;
                if (f <= last_f || f * 144.0 / dpi > 1151) p.f_order_bad = 1;
                last_f = f;
                x = f * 144.0 / dpi;
                x_known = 1;
                break;
            }
            case 'G': case 'V': {
                int n = num(b + i, 4); i += 4;
                if (k == 'V' && b[i] == 0x01) p.rule_top_pin_only = 1;
                i += k == 'G' ? (size_t)n : 1;
                printed = 1;
                if (p.first_y < 0) p.first_y = y;
                x += n * 144.0 / dpi * (dw ? 2 : 1);
                if (x > p.max_x) p.max_x = x;
                if (y + 16 > p.max_y) p.max_y = y + 16;
                break;
            }
            default: p.bad = 1; break;
            }
        } else if (c == '\r') {
            x = 0; last_f = -1; x_known = 1;
            passes += printed;
            printed = 0;
            if (line_nlq && passes > 1) p.nlq_overprint = 1;
        } else if (c == '\n') {
            y += lf;
            passes = printed = line_nlq = 0;
        } else if (c == 0x0C) {
            break;
        } else if (c == 0x0E) {
            dw = 1;
        } else if (c == 0x0F) {
            dw = 0;
        } else if (c >= 0x20 && c < 0x7F) {
            if (c != ' ') {
                printed = 1;
                if (p.first_y < 0) p.first_y = y;
                line_nlq |= nlq;
                if (line_nlq && passes) p.nlq_overprint = 1;
            }
            if (prop) x_known = 0;
            else {
                x += 144.0 / cpi * (dw ? 2 : 1);
                if (x_known && x > p.max_x) p.max_x = x;
            }
            if (y + 18 > p.max_y) p.max_y = y + 18;
        }
    }
    return p;
}

static void test_page_frame(void)
{
    tp_info_t info = info_for(IW_RIBBON_COLOUR);
    tp_result_t rc;
    buf_t s = print_page(&info, &rc);
    CHECK_EQ_INT(rc, TP_DONE);
    CHECK(s.n > 20000);
    CHECK(s.n > 2 && s.b[0] == 0x1B && s.b[1] == 'c');
    CHECK(s.n && s.b[s.n - 1] == 0x0C);
    iw_scan_t sc;
    iw_scan_init(&sc);
    iw_scan(&sc, s.b, s.n);
    CHECK(iw_scan_between(&sc));
    free(s.b);
}

static void test_page_fits(void)
{
    for (int r = IW_RIBBON_UNKNOWN; r <= IW_RIBBON_COLOUR; r++) {
        tp_info_t info = info_for((iw_ribbon_t)r);
        tp_result_t rc;
        buf_t s = print_page(&info, &rc);
        page_t p = read_page(&s);
        CHECK_EQ_INT(p.bad, 0);
        CHECK_EQ_INT(p.f_order_bad, 0);
        CHECK(p.max_x <= 1152.001);
        CHECK(p.max_y <= 1512);             // 10.5 in: inside the bottom margin
        CHECK(p.max_y > 1300);              // a full page, not a strip
        free(s.b);
    }
}

static void test_page_inks(void)
{
    tp_info_t info = info_for(IW_RIBBON_COLOUR);
    tp_result_t rc;
    buf_t s = print_page(&info, &rc);
    page_t p = read_page(&s);
    for (int d = 0; d < 7; d++) CHECK(p.k_used[d]);
    CHECK_EQ_INT(p.last_k, 0);              // the page leaves black selected
    CHECK(has(&s, "Colour"));
    free(s.b);

    for (int r = IW_RIBBON_UNKNOWN; r <= IW_RIBBON_BLACK; r++) {
        info = info_for((iw_ribbon_t)r);
        s = print_page(&info, &rc);
        p = read_page(&s);
        CHECK_EQ_INT(p.k_any, 0);
        CHECK(has(&s, "Photo"));
        CHECK(has(&s, "could not tell") == (r == IW_RIBBON_UNKNOWN));
        CHECK(has(&s, "black ribbon is in") == (r == IW_RIBBON_BLACK));
        free(s.b);
    }
}

static void test_page_identity(void)
{
    tp_info_t info = info_for(IW_RIBBON_COLOUR);
    tp_result_t rc;
    buf_t s = print_page(&info, &rc);
    CHECK(has(&s, "AirWriter v0.2.0-29-g4a93f60"));
    CHECK(has(&s, "Oct 20 2026"));
    CHECK(has(&s, "imagewriter-2434.local  10.0.1.115"));
    CHECK(has(&s, "http://imagewriter-2434.local"));
    CHECK(has(&s, "ImageWriter II, colour ribbon"));
    CHECK(has(&s, "AppleTalk port"));
    free(s.b);

    /* DIP4 at power-on can print before the network gives an address */
    info.ip = "0.0.0.0";
    s = print_page(&info, &rc);
    CHECK(has(&s, "imagewriter-2434.local  no IP yet"));
    CHECK(!has(&s, "0.0.0.0"));
    free(s.b);

    info.setup_ssid = "AirWriter-2434";
    info.ip = "192.168.4.1";
    s = print_page(&info, &rc);
    CHECK(has(&s, "join AirWriter-2434"));
    CHECK(has(&s, "http://192.168.4.1"));
    CHECK(!has(&s, "imagewriter-2434.local"));
    CHECK(!has(&s, "0.0.0.0"));
    free(s.b);
}

static void test_page_tick(void)
{
    static const struct { ht_algo_t a; const char *name; } sc[] = {
        { HT_ATKINSON, "Atkinson" }, { HT_FLOYD, "Floyd-Steinberg" },
        { HT_BLUENOISE, "Blue noise" }, { HT_BAYER, "Ordered" }, { HT_THRESHOLD, "Threshold" },
    };
    for (size_t k = 0; k < sizeof sc / sizeof sc[0]; k++) {
        tp_info_t info = info_for(IW_RIBBON_BLACK);
        info.screen = sc[k].a;
        tp_result_t rc;
        buf_t s = print_page(&info, &rc);
        CHECK_EQ_INT(count(&s, "\x1b&D", 3), 1);
        char want[64];
        snprintf(want, sizeof want, "\x1b!%s\x1b\" \x1b&D", sc[k].name);
        CHECK(has(&s, want));
        free(s.b);
    }
}

/* Apple's screen is a StyleWriter's, but a name for it is the screen's own: the details line
 * never reads "Threshold" for it (a default case once did). */
static void test_page_names_every_screen(void)
{
    static const struct { ht_algo_t a; const char *name; } sc[] = {
        { HT_ATKINSON, "Atkinson" }, { HT_FLOYD, "Floyd-Steinberg" }, { HT_BLUENOISE, "Blue noise" },
        { HT_BAYER, "Ordered" }, { HT_THRESHOLD, "Threshold" }, { HT_APPLE, "Apple" },
    };
    for (size_t k = 0; k < sizeof sc / sizeof sc[0]; k++) {
        tp_info_t info = info_for(IW_RIBBON_BLACK);
        info.screen = sc[k].a;
        tp_result_t rc;
        buf_t s = print_page(&info, &rc);
        char want[64];
        snprintf(want, sizeof want, "Halftone   %s", sc[k].name);
        CHECK(has(&s, want));
        free(s.b);
    }
}

static void test_page_long_names(void)
{
    tp_info_t info = info_for(IW_RIBBON_COLOUR);
    info.host = "a-very-long-host-name-for-an-imagewriter-on-a-big-network-xy";
    info.name = "The Office ImageWriter II By The Window";
    info.version = "v10.20.300-4000-gdeadbeef-dirty-and-then-some";
    info.appletalk = "AppleTalk port, networked Macs, Basilisk II, Infinite Mac";
    tp_result_t rc;
    buf_t s = print_page(&info, &rc);
    page_t p = read_page(&s);
    CHECK_EQ_INT(rc, TP_DONE);
    CHECK(p.max_x <= 1152.001);
    free(s.b);
}

/* A name typed on a phone: the printer prints 7-bit ASCII, so each other
 * character is one '?', and no byte of it reaches the printer as text. */
static void test_page_names_outside_ascii(void)
{
    tp_info_t info = info_for(IW_RIBBON_BLACK);
    info.name = "Wells\xe2\x80\x99s \x1b" "c printer";
    tp_result_t rc;
    buf_t s = print_page(&info, &rc);
    CHECK(has(&s, "Wells?s ?c printer"));
    CHECK(!has(&s, "Wells\xe2"));
    free(s.b);
}

/* Board 1's first print (2026-10-20) put whole strips and colour passes
 * inches to the right, and wrapped an ESC F-placed "8" to the left margin.
 * AirPrint's streams, which print right, never place the head; neither does
 * the page. */
static void test_page_no_head_placement(void)
{
    for (int r = IW_RIBBON_UNKNOWN; r <= IW_RIBBON_COLOUR; r++) {
        tp_info_t info = info_for((iw_ribbon_t)r);
        tp_result_t rc;
        buf_t s = print_page(&info, &rc);
        page_t p = read_page(&s);
        CHECK_EQ_INT(p.f_used, 0);
        CHECK_EQ_INT(p.nlq_overprint, 0);       // NLQ's own passes, never CR-only over them
        CHECK_EQ_INT(p.rule_top_pin_only, 0);   // board 1's top pin is dead
        free(s.b);
    }
}

/* Every AppleTalk way in, named in full: the line under the right column
 * goes on to a fifth row rather than being cut. */
static void test_page_appletalk_in_full(void)
{
    tp_info_t info = info_for(IW_RIBBON_COLOUR);
    info.appletalk = "AppleTalk port, networked Macs, Basilisk II, Infinite Mac";
    tp_result_t rc;
    buf_t s = print_page(&info, &rc);
    page_t p = read_page(&s);
    CHECK(has(&s, "AppleTalk port, networked Macs,"));
    CHECK(has(&s, "Basilisk II, Infinite Mac"));
    CHECK(p.max_x <= 1152.001);
    CHECK(p.max_y <= 1512);
    free(s.b);
}

/* The page is laid out as an AirPrint page is: from the sheet's top edge,
 * its content from 1 in down. The top margin setting moves it as it moves an
 * AirPrint page, but drops no more than that blank inch. */
static void test_page_top_margin(void)
{
    static const struct { int top, first_y; } cases[] = {
        { 0, 144 }, { -144, 0 }, { 72, 216 }, { -9, 135 }, { -216, 0 }, { 144, 288 },
    };
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        tp_info_t info = info_for(IW_RIBBON_COLOUR);
        info.top_margin = cases[k].top;
        tp_result_t rc;
        buf_t s = print_page(&info, &rc);
        page_t p = read_page(&s);
        CHECK_EQ_INT(rc, TP_DONE);
        CHECK_EQ_INT(p.first_y, cases[k].first_y);
        free(s.b);
    }
}

static int stop_calls;
static int stop_on_10th(void *ctx) { (void)ctx; return ++stop_calls >= 10; }

static void test_page_stops(void)
{
    tp_info_t info = info_for(IW_RIBBON_COLOUR);
    buf_t s = { 0 };
    tp_io_t io = { .out = put, .out_ctx = &s, .stop = stop_on_10th };
    stop_calls = 0;
    tp_result_t rc = tp_print(&info, &io);
    CHECK_EQ_INT(rc, TP_STOPPED);
    CHECK_EQ_INT(stop_calls, 10);
    CHECK(s.n > 0 && s.b[s.n - 1] != 0x0C);
    CHECK(s.n < 12000);
    free(s.b);
}

int main(void)
{
    test_scene();
    test_page_frame();
    test_page_fits();
    test_page_inks();
    test_page_identity();
    test_page_tick();
    test_page_names_every_screen();
    test_page_long_names();
    test_page_names_outside_ascii();
    test_page_no_head_placement();
    test_page_appletalk_in_full();
    test_page_top_margin();
    test_page_stops();
    TEST_END;
}
