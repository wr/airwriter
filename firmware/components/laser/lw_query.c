// SPDX-License-Identifier: GPL-3.0-or-later
#include "lw_query.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Level 1 throughout. awq prints aw:<key>= and runs its procedure inside
// stopped, so a question the printer cannot answer prints "?" and the job
// goes on; awp prints any value. The probe's procedure runs when awq does, so
// its 256 bytes follow the awq that starts it, after exactly one LF.
static const char k_head[] =
    "%!PS-Adobe-3.0\n"
    "%%Title: AirWriter query\n"
    "%%Creator: AirWriter\n"
    "%%EndComments\n"
    "/aws 64 string def\n"
    "/awp { dup type /stringtype ne { aws cvs } if print } bind def\n"
    "/awq { exch (aw:) print print (=) print\n"
    "  mark exch stopped { cleartomark (?) print } { cleartomark } ifelse\n"
    "  (\\n) print flush } bind def\n"
    "(product) { statusdict /product get awp } awq\n"
    "(version) { version awp } awq\n"
    "(revision) { statusdict /revision get awp } awq\n"
    "(level) { /languagelevel where { pop languagelevel } { 1 } ifelse awp } awq\n"
    "(res) { 72 0 matrix defaultmatrix dtransform dup mul exch dup mul add sqrt round cvi awp\n"
    "  ( ) print 0 72 matrix defaultmatrix dtransform dup mul exch dup mul add sqrt round cvi awp } awq\n"
    "(area) { gsave initclip clippath pathbbox grestore 4 -1 roll awp ( ) print\n"
    "  3 -1 roll awp ( ) print exch awp ( ) print awp } awq\n"
    "(size) { currentpagedevice /PageSize get aload pop exch awp ( ) print awp } awq\n"
    "(pages) { statusdict /pagecount get exec awp } awq\n"
    // An operator on a LaserWriter, a boolean in Ghostscript.
    "(startpage) { statusdict /dostartpage get dup type /booleantype ne { exec } if awp } awq\n"
    "(probe) { /awb 256 string def currentfile awb readstring pop length awp ( ) print\n"
    "  true 0 1 255 { dup awb exch get ne { pop false } if } for { (1) } { (0) } ifelse print } awq\n";
static const char k_tail[] =
    "\n(aw:end\\n) print flush\n"
    "%%EOF\n";
// After the probe, a line of spaces as long as it: a channel that drops
// probe bytes leaves readstring eating spaces, not the end of the job.
#define PAD 256

// After exitserver nothing is undone at the job's end, so no defs: only the
// setting, then its answer read back.
static const char k_sp_head[] =
    "%!PS-Adobe-3.0\n"
    "%%Title: AirWriter start page\n"
    "%%Creator: AirWriter\n"
    "%%EndComments\n"
    "serverdict begin 0 exitserver\n"
    "statusdict begin ";
static const char k_sp_tail[] =
    " setdostartpage end\n"
    "(aw:startpage=) print\n"
    "statusdict /dostartpage get dup type /booleantype ne { exec } if\n"
    "{ (true) } { (false) } ifelse print (\\n) print\n"
    "(aw:end\\n) print flush\n"
    "%%EOF\n";

size_t lw_start_page_job(uint8_t *out, size_t cap, int on)
{
    const char *v = on ? "true" : "false";
    size_t h = sizeof k_sp_head - 1, n = strlen(v), t = sizeof k_sp_tail - 1;
    if (cap < h + n + t) return 0;
    memcpy(out, k_sp_head, h);
    memcpy(out + h, v, n);
    memcpy(out + h + n, k_sp_tail, t);
    return h + n + t;
}

size_t lw_query_job(uint8_t *out, size_t cap)
{
    size_t h = sizeof k_head - 1, t = sizeof k_tail - 1;
    if (cap < h + 256 + PAD + t) return 0;
    memcpy(out, k_head, h);
    for (int i = 0; i < 256; i++) out[h + (size_t)i] = (uint8_t)i;
    memset(out + h + 256, ' ', PAD);
    memcpy(out + h + 256 + PAD, k_tail, t);
    return h + 256 + PAD + t;
}

void lw_info_default(lw_info_t *i)
{
    memset(i, 0, sizeof *i);
    strcpy(i->product, "LaserWriter");
    i->level = 1;
    i->dpi = 300;
    i->page = LW_LETTER;
    i->area[0] = 1800;  i->area[1] = 1800;      // a quarter inch in from each edge
    i->area[2] = 59400; i->area[3] = 77400;     // of 612 x 792
}

const char *lw_page_name(lw_page_t p)
{
    return p == LW_A4 ? "a4" : p == LW_LEGAL ? "legal" : "letter";
}

void lw_parse_init(lw_parse_t *p)
{
    memset(p, 0, sizeof *p);
    lw_info_default(&p->info);
    p->pages = -1;
}

// Up to n numbers from s; returns how many there were.
static int numbers(const char *s, double *v, int n)
{
    int k = 0;
    while (k < n) {
        char *end;
        double d = strtod(s, &end);
        if (end == s) break;
        v[k++] = d;
        s = end;
    }
    return k;
}

static int32_t hundredths(double pt)
{
    return (int32_t)(pt * 100.0 + (pt < 0 ? -0.5 : 0.5));
}

// The paper whose height the imageable area reaches: what a Level 1 machine,
// which has no PageSize, is taken to hold.
static lw_page_t page_from_area(const int32_t area[4])
{
    return area[3] > 84200 ? LW_LEGAL : area[3] > 79200 ? LW_A4 : LW_LETTER;
}

static void answer(lw_parse_t *p, const char *key, const char *val)
{
    lw_info_t *i = &p->info;
    if (!strcmp(key, "end")) {
        i->complete = 1;
        return;
    }
    if (!strcmp(val, "?") || !val[0]) {                 // unanswered: the default stands
        if (!strcmp(key, "probe")) i->complete = 1;
        return;
    }
    double v[4];
    if (!strcmp(key, "product")) {
        strncpy(i->product, val, sizeof i->product - 1);
        i->product[sizeof i->product - 1] = '\0';
    } else if (!strcmp(key, "version")) {
        strncpy(i->version, val, sizeof i->version - 1);
        i->version[sizeof i->version - 1] = '\0';
    } else if (!strcmp(key, "revision")) {
        i->revision = (int32_t)strtol(val, NULL, 10);
    } else if (!strcmp(key, "level")) {
        long l = strtol(val, NULL, 10);
        if (l >= 1 && l <= 3) i->level = (uint8_t)l;
    } else if (!strcmp(key, "res")) {
        int k = numbers(val, v, 2);
        double d = k == 2 && v[1] > v[0] ? v[1] : v[0];
        if (k && d >= 72 && d <= 2400) i->dpi = (uint16_t)(d + 0.5);
    } else if (!strcmp(key, "area")) {
        if (numbers(val, v, 4) == 4 && v[2] > v[0] && v[3] > v[1]) {
            for (int k = 0; k < 4; k++) i->area[k] = hundredths(v[k]);
            if (!p->sized) i->page = page_from_area(i->area);
        }
    } else if (!strcmp(key, "size")) {
        if (numbers(val, v, 2) == 2) {
            double w = v[0] < v[1] ? v[0] : v[1], h = v[0] < v[1] ? v[1] : v[0];
            p->sized = 1;
            i->page = h > 900 ? LW_LEGAL : (w < 604 && h > 800) ? LW_A4 : LW_LETTER;
        }
    } else if (!strcmp(key, "pages")) {
        char *end;
        long v = strtol(val, &end, 10);
        if (end != val && !*end && v >= 0 && v <= INT32_MAX) p->pages = (int32_t)v;
    } else if (!strcmp(key, "startpage")) {
        if (!strcmp(val, "true")) i->start_page = LW_START_ON;
        else if (!strcmp(val, "false")) i->start_page = LW_START_OFF;
    } else if (!strcmp(key, "probe")) {
        int k = numbers(val, v, 2);
        i->binary = k == 2 && v[0] == 256 && v[1] == 1;
        // Every other question comes before it: a printer that mangles
        // binary may choke on what follows, but has answered (review I2).
        i->complete = 1;
    }
}

static void line_done(lw_parse_t *p)
{
    p->line[p->n] = '\0';
    p->n = 0;
    // W-1099: exitserver's answer to a password other than 0.
    if (!strncmp(p->line, "%%[", 3) && strstr(p->line, "invalidaccess")) p->refused = 1;
    if (strncmp(p->line, "aw:", 3)) return;             // %%[ status ]%% and the like
    char *eq = strchr(p->line + 3, '=');
    if (!eq) {
        if (!strcmp(p->line + 3, "end")) answer(p, "end", "");
        return;
    }
    *eq = '\0';
    char *val = eq + 1;
    size_t len = strlen(val);
    while (len && (val[len - 1] == ' ' || val[len - 1] == '\t')) val[--len] = '\0';
    answer(p, p->line + 3, val);
}

void lw_parse_feed(lw_parse_t *p, const uint8_t *d, size_t n)
{
    for (size_t k = 0; k < n; k++) {
        uint8_t c = d[k];
        if (c == '\r' || c == '\n') {
            if (p->n) line_done(p);
        } else if (p->n < sizeof p->line - 1) {
            p->line[p->n++] = (char)c;
        }
    }
}
