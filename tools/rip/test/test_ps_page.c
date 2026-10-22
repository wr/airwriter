// SPDX-License-Identifier: GPL-3.0-or-later
//
// ps_page (W-1082): 1-bit rows into Level 1 PostScript for a LaserWriter.
// The shape of the file, here; that Ghostscript paints exactly the rip's
// dots is pscheck.py's job (make -C tools/rip pscheck).
#include "test_util.h"
#include "ps_page.h"
#include <stdlib.h>
#include <ctype.h>

static uint8_t out[1 << 20];
static size_t  out_n;
static void put(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (out_n + n <= sizeof out) memcpy(out + out_n, b, n);
    out_n += n;
}

typedef struct { int x, y, w, h, wb; size_t data; } band_t;

// strstr over the output, which holds binary data and so NULs.
static const char *find(const char *from, const char *needle)
{
    const char *end = (const char *)out + out_n;
    size_t k = strlen(needle);
    for (const char *p = from; p + k <= end; p++)
        if (memcmp(p, needle, k) == 0) return p;
    return NULL;
}

// Every band call after the prolog: its operands and where its data starts.
static int bands(band_t *b, int max, int binary)
{
    const char *p = find((const char *)out, "%%EndProlog");
    int n = 0;
    while (p && n < max) {
        const char *hit = find(p, " awb\n");
        if (!hit) break;
        const char *line = hit;
        while (line > (const char *)out && line[-1] != '\n') line--;
        band_t *k = &b[n];
        if (sscanf(line, "%d %d %d %d %d", &k->x, &k->y, &k->w, &k->h, &k->wb) != 5) break;
        k->data = (size_t)(hit + 5 - (const char *)out);
        n++;
        p = hit + 5 + (binary ? (size_t)k->h * (size_t)k->wb : 0);
    }
    return n;
}

static int count(const char *needle)
{
    int n = 0;
    for (const char *p = (const char *)out; (p = find(p, needle)); p++) n++;
    return n;
}

static ps_t *job(int binary, const char *title)
{
    ps_cfg_t c = { .out = put, .binary = binary, .paper_h_pt = 792,
                   .title = title, .creator = "AirWriter test" };
    out_n = 0;
    memset(out, 0, sizeof out);
    ps_t *ps = ps_new(&c);
    ps_job_begin(ps);
    return ps;
}

// A w-pixel page of h rows; ink(y, row) fills a row of nb bytes.
static void page(ps_t *ps, int index, uint32_t w, uint32_t h, void (*ink)(uint32_t y, uint8_t *row))
{
    rip_stage_t st;
    ps_stage(ps, &st);
    static uint8_t row[512];
    CHECK_EQ_INT(st.page_begin(st.ctx, index, w, h), 0);
    for (uint32_t y = 0; y < h; y++) {
        memset(row, 0, sizeof row);
        if (ink) ink(y, row);
        st.row(st.ctx, row);
    }
    CHECK_EQ_INT(st.page_end(st.ctx), 0);
}

// Rows 0-63 ink bytes 2..4; 64 and 65 blank; 66-129 ink byte 10.
static void two_bands(uint32_t y, uint8_t *row)
{
    if (y < 64) { row[2] = 0x80; row[3] = (uint8_t)y; row[4] = 0x01; }
    else if (y >= 66) row[10] = (uint8_t)(0x40 | (y & 7));
}

static void last_row_ink(uint32_t y, uint8_t *row) { if (y >= 95) row[0] = 0xFF; }
static void full_width(uint32_t y, uint8_t *row) { (void)y; memset(row, 0xA5, 512); }

static int hexval(int c) { return isdigit(c) ? c - '0' : tolower(c) - 'a' + 10; }

int main(void)
{
    // Header, prolog, title escaping, trailer.
    ps_t *ps = job(1, "a (b) \\c \xc3\xa9");
    page(ps, 0, 2550, 130, two_bands);
    page(ps, 1, 2550, 8, NULL);
    ps_job_end(ps);
    CHECK(memcmp(out, "%!PS-Adobe-3.0\n", 15) == 0);
    CHECK(count("%%Creator: AirWriter test\n") == 1);
    CHECK(count("%%Title: (a \\(b\\) \\\\c \\303\\251)\n") == 1);
    CHECK(count("%%LanguageLevel: 1\n") == 1);
    CHECK(count("%%EndProlog\n") == 1);
    CHECK(count(" bind def\n%%EndProlog\n") == 1);      // every prolog line whole
    CHECK(count("%%Page: 1 1\n") == 1 && count("%%Page: 2 2\n") == 1);
    CHECK(count("save\n") == 2 && count("restore showpage\n") == 2);
    CHECK(count(" awb\r") == 0);
    const char *tail = "%%Trailer\n%%Pages: 2\n%%EOF\n";
    CHECK(out_n > strlen(tail) && memcmp(out + out_n - strlen(tail), tail, strlen(tail)) == 0);

    // Bands: 64 rows, the blank pair skipped, 64 more; trimmed to the ink.
    band_t b[8];
    int nb = bands(b, 8, 1);
    CHECK_EQ_INT(nb, 2);
    CHECK_EQ_INT(b[0].x, 16); CHECK_EQ_INT(b[0].y, 0);  CHECK_EQ_INT(b[0].h, 64);
    CHECK_EQ_INT(b[0].wb, 3); CHECK_EQ_INT(b[0].w, 24);
    CHECK_EQ_INT(b[1].x, 80); CHECK_EQ_INT(b[1].y, 66); CHECK_EQ_INT(b[1].h, 64);
    CHECK_EQ_INT(b[1].wb, 1); CHECK_EQ_INT(b[1].w, 8);
    // Binary: one LF after the name, then exactly the rows' bytes.
    CHECK_EQ_INT(out[b[0].data - 1], '\n');
    CHECK_EQ_INT(out[b[0].data - 2], 'b');
    CHECK_EQ_INT(out[b[0].data + 5 * 3 + 0], 0x80);   // row 5, byte 2
    CHECK_EQ_INT(out[b[0].data + 5 * 3 + 1], 5);
    CHECK_EQ_INT(out[b[0].data + 5 * 3 + 2], 0x01);
    CHECK_EQ_INT(out[b[1].data + 0], 0x40 | (66 & 7));
    CHECK_EQ_INT(out[b[1].data + 63], 0x40 | (129 & 7));
    CHECK_EQ_INT(out[b[1].data + 64], '\n');          // the next line follows the data
    ps_free(ps);

    // An empty page still prints (Review Focus 2), with no band in it.
    ps = job(1, "blank");
    page(ps, 0, 2550, 3300, NULL);
    ps_job_end(ps);
    CHECK_EQ_INT(bands(b, 8, 1), 0);
    CHECK(count("restore showpage\n") == 1);
    ps_free(ps);

    // Ink down to the last row: that band is flushed at the page's end.
    ps = job(1, "last");
    page(ps, 0, 64, 100, last_row_ink);
    ps_job_end(ps);
    CHECK_EQ_INT(bands(b, 8, 1), 1);
    CHECK_EQ_INT(b[0].y, 95);
    CHECK_EQ_INT(b[0].h, 5);
    ps_free(ps);

    // Hex: no line over 64 digits, and the digits are the rows.
    ps = job(0, "hex");
    page(ps, 0, 2550, 130, two_bands);
    ps_job_end(ps);
    CHECK(count("readhexstring") >= 1 && count("readstring") == 0);
    nb = bands(b, 8, 0);
    CHECK_EQ_INT(nb, 2);
    {
        const char *p = (const char *)out + b[0].data;
        uint8_t got[64 * 3];
        size_t k = 0, run = 0, longest = 0;
        while (k < sizeof got) {
            if (*p == '\n') { if (run > longest) longest = run; run = 0; p++; continue; }
            got[k++] = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
            p += 2;
            run += 2;
        }
        CHECK(longest <= 64);
        CHECK_EQ_INT(got[5 * 3 + 1], 5);
        CHECK_EQ_INT(got[63 * 3 + 1], 63);
        size_t lines = 0, maxl = 0, cur = 0;
        for (size_t i = b[0].data; i < out_n && out[i] != '%'; i++) {
            if (out[i] == '\n') { lines++; if (cur > maxl) maxl = cur; cur = 0; }
            else if (isxdigit(out[i])) cur++;
            else break;
        }
        CHECK(maxl <= 64 && lines >= 6);
    }
    ps_free(ps);

    // 2560 px (PS_ROW_MAX bytes) is accepted whole; 3000 px is clipped to it.
    ps = job(1, "wide");
    page(ps, 0, 2560, 2, full_width);
    page(ps, 1, 3000, 2, full_width);
    ps_job_end(ps);
    CHECK_EQ_INT(bands(b, 8, 1), 2);
    CHECK_EQ_INT(b[0].wb, PS_ROW_MAX);
    CHECK_EQ_INT(b[1].wb, PS_ROW_MAX);
    CHECK_EQ_INT(b[1].w, PS_ROW_MAX * 8);
    CHECK_EQ_INT(out[b[1].data + 2 * PS_ROW_MAX], '\n');
    ps_free(ps);

    TEST_END;
}
