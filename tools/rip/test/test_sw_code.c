// SPDX-License-Identifier: GPL-3.0-or-later
//
// sw_code (W-1103): the StyleWriter's row run coder and its decoder. A row
// is coded plane by plane; each plane row is XORed with the one before in the
// band, so "all zero after the XOR" is one byte, 80.
#include "test_util.h"
#include "sw_code.h"
#include <stdlib.h>

#define ROW 365                  // 2919 dots, rounded up to bytes

enum { C_LIT = 0x00, C_REP = 0x40, C_WHITE = 0x80, C_BLACK = 0xC0 };

static uint32_t rng_s = 0x57ec0de5;
static uint32_t rnd(void)        // xorshift32: the same rows on every host
{
    rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5;
    return rng_s;
}

// Walks a coded row with no help from the decoder. Returns the bytes the codes
// make (0 when malformed), and checks the counts as it goes: 1 to 62, except
// for the lone 80, which is the whole row of `rowbytes`.
static size_t walk(const uint8_t *c, size_t n, size_t rowbytes)
{
    if (n == 1 && c[0] == C_WHITE) return rowbytes;
    size_t i = 0, made = 0;
    while (i < n) {
        uint8_t h = c[i++];
        unsigned cnt = h & 63;
        CHECK(cnt >= 1 && cnt <= 62);
        if (cnt < 1 || cnt > 62) return 0;
        switch (h & 0xC0) {
        case C_LIT: i += cnt; break;
        case C_REP: i += 1; break;
        default: break;
        }
        made += cnt;
    }
    CHECK_EQ_INT(i, n);
    return i == n ? made : 0;
}

static size_t code(const uint8_t *row, const uint8_t *prev, size_t n, uint8_t *out)
{
    // The buffer is exactly sw_code_max long with a guard behind it.
    size_t cap = sw_code_max(n);
    uint8_t *buf = malloc(cap + 8);
    memset(buf + cap, 0xA5, 8);
    size_t got = sw_code_row(row, prev, n, buf);
    CHECK(got <= cap);
    for (int i = 0; i < 8; i++) CHECK_EQ_INT(buf[cap + i], 0xA5);
    if (got <= cap) memcpy(out, buf, got);
    free(buf);
    return got;
}

static void test_known_rows(void)
{
    uint8_t row[ROW], prev[ROW], out[sizeof row * 2];

    memset(row, 0x00, sizeof row);
    CHECK_EQ_INT(code(row, NULL, ROW, out), 1);
    CHECK_EQ_INT(out[0], 0x80);

    // A row equal to the one before: nothing changed.
    for (size_t i = 0; i < ROW; i++) row[i] = prev[i] = (uint8_t)(i * 37 + 5);
    CHECK_EQ_INT(code(row, prev, ROW, out), 1);
    CHECK_EQ_INT(out[0], 0x80);
    // A zero row after a zero row, too.
    memset(row, 0, sizeof row); memset(prev, 0, sizeof prev);
    CHECK_EQ_INT(code(row, prev, ROW, out), 1);
    CHECK_EQ_INT(out[0], 0x80);

    // 365 black bytes: runs of FF-class codes of 62 at most, summing to 365.
    memset(row, 0xFF, sizeof row);
    size_t n = code(row, NULL, ROW, out);
    size_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ_INT(out[i] & 0xC0, C_BLACK);
        CHECK(((out[i] & 63) >= 1) && ((out[i] & 63) <= 62));
        sum += out[i] & 63;
    }
    CHECK_EQ_INT(sum, ROW);
    CHECK_EQ_INT(n, 6);                       // five of 62 and one of 55
    CHECK_EQ_INT(out[0], 0xFE);
    CHECK_EQ_INT(out[5], 0xC0 | 55);

    // Against a prev of all FF it is a zero row again.
    memset(prev, 0xFF, sizeof prev);
    CHECK_EQ_INT(code(row, prev, ROW, out), 1);
    CHECK_EQ_INT(out[0], 0x80);
    // And a white row against a prev of FF is black: the delta is the row.
    memset(row, 0x00, sizeof row);
    n = code(row, prev, ROW, out);
    CHECK_EQ_INT(n, 6);
    CHECK_EQ_INT(out[0], 0xFE);

    // The four kinds of code, in order: white pair, repeat (with the row's
    // last bytes after it), literal, black run.
    static const uint8_t a[] = { 0x00, 0x00, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,
                                 0x12, 0x34, 0xFF, 0xFF, 0xFF };
    static const uint8_t a_want[] = { 0x82, 0x46, 0xAA, 0x02, 0x12, 0x34, 0xC3 };
    n = code(a, NULL, sizeof a, out);
    CHECK_EQ_INT(n, sizeof a_want);
    CHECK_EQ_MEM(out, a_want, sizeof a_want);

    // A repeat needs 3 equal bytes and 5 left in the row.
    static const uint8_t b[] = { 0x12, 0x12, 0x12, 0x34, 0x56 };
    static const uint8_t b_want[] = { 0x43, 0x12, 0x02, 0x34, 0x56 };
    n = code(b, NULL, sizeof b, out);
    CHECK_EQ_INT(n, sizeof b_want);
    CHECK_EQ_MEM(out, b_want, sizeof b_want);
    static const uint8_t c[] = { 0x12, 0x12, 0x12, 0x34 };      // only 4 left
    static const uint8_t c_want[] = { 0x04, 0x12, 0x12, 0x12, 0x34 };
    n = code(c, NULL, sizeof c, out);
    CHECK_EQ_INT(n, sizeof c_want);
    CHECK_EQ_MEM(out, c_want, sizeof c_want);

    // One 00 is not a run: it rides in the literal.
    static const uint8_t d[] = { 0x12, 0x00, 0x34 };
    static const uint8_t d_want[] = { 0x03, 0x12, 0x00, 0x34 };
    n = code(d, NULL, sizeof d, out);
    CHECK_EQ_INT(n, sizeof d_want);
    CHECK_EQ_MEM(out, d_want, sizeof d_want);

    // A run past 62 splits: 100 white bytes, then one literal byte.
    uint8_t e[101];
    memset(e, 0, sizeof e); e[100] = 0x12;
    static const uint8_t e_want[] = { 0xBE, 0xA6, 0x01, 0x12 };
    n = code(e, NULL, sizeof e, out);
    CHECK_EQ_INT(n, sizeof e_want);
    CHECK_EQ_MEM(out, e_want, sizeof e_want);
    // Sixty-three white bytes leave one that is no pair: it joins the literal.
    memset(e, 0, 64); e[63] = 0x12;
    static const uint8_t f_want[] = { 0xBE, 0x02, 0x00, 0x12 };
    n = code(e, NULL, 64, out);
    CHECK_EQ_INT(n, sizeof f_want);
    CHECK_EQ_MEM(out, f_want, sizeof f_want);

    // 70 bytes that never repeat: a literal of 62 and one of 8.
    uint8_t g[70];
    for (size_t i = 0; i < sizeof g; i++) g[i] = (uint8_t)(i * 3 + 1);
    n = code(g, NULL, sizeof g, out);
    CHECK_EQ_INT(n, 70 + 2);
    CHECK_EQ_INT(out[0], 62);
    CHECK_EQ_INT(out[63], 8);
    CHECK_EQ_MEM(out + 1, g, 62);
    CHECK_EQ_MEM(out + 64, g + 62, 8);
}

// Rows of many kinds: noise, a few levels, long runs, mostly the row before.
static void fill_row(uint8_t *row, const uint8_t *prev, size_t n, unsigned kind)
{
    switch (kind % 6) {
    case 0:                                    // noise
        for (size_t i = 0; i < n; i++) row[i] = (uint8_t)rnd();
        break;
    case 1:                                    // a handful of levels
        for (size_t i = 0; i < n; i++) {
            static const uint8_t lv[] = { 0x00, 0xFF, 0xAA, 0x55, 0x0F };
            row[i] = lv[rnd() % 5];
        }
        break;
    case 2:                                    // long runs of a few values
        for (size_t i = 0; i < n;) {
            static const uint8_t lv[] = { 0x00, 0xFF, 0x3C, 0x81 };
            uint8_t v = lv[rnd() % 4];
            size_t len = 1 + rnd() % 150;
            for (; len && i < n; len--) row[i++] = v;
        }
        break;
    case 3:                                    // sparse ink
        for (size_t i = 0; i < n; i++) row[i] = (rnd() % 9 == 0) ? (uint8_t)rnd() : 0;
        break;
    case 4:                                    // the row before, a few bytes changed
        for (size_t i = 0; i < n; i++) row[i] = prev ? prev[i] : 0;
        for (unsigned k = rnd() % 6; k; k--) row[rnd() % n] = (uint8_t)rnd();
        break;
    default:                                   // short repeats of 2 to 6
        for (size_t i = 0; i < n;) {
            uint8_t v = (uint8_t)rnd();
            size_t len = 2 + rnd() % 5;
            for (; len && i < n; len--) row[i++] = v;
        }
        break;
    }
}

static void test_round_trip(void)
{
    static uint8_t row[ROW + 40], prev[ROW + 40], out[ROW * 2], back[ROW + 40];
    size_t worst = 0;
    for (int iter = 0; iter < 1000; iter++) {
        size_t n = (iter % 7 == 0) ? 1 + rnd() % ROW : ROW;
        int use_prev = iter % 2;
        fill_row(prev, NULL, n, (unsigned)rnd());
        fill_row(row, prev, n, (unsigned)(iter / 2));
        const uint8_t *p = use_prev ? prev : NULL;

        size_t len = code(row, p, n, out);
        CHECK(len >= 1);
        if (len > worst) worst = len;
        CHECK_EQ_INT(walk(out, len, n), n);                  // the codes make the row, no more
        memset(back, 0x5A, sizeof back);
        size_t used = sw_decode_row(out, len, p, n, back);
        CHECK_EQ_INT(used, len);
        CHECK_EQ_MEM(back, row, n);
        // Extra bytes behind the codes are the next row's, not this one's.
        out[len] = 0x7E;
        CHECK_EQ_INT(sw_decode_row(out, len + 1, p, n, back), len);
    }
    CHECK(worst <= sw_code_max(ROW));
}

// Rows meant to be the worst for size: no byte ever matches its neighbour,
// or only in pairs and triples a rule would split on.
static void test_worst_case(void)
{
    static uint8_t row[ROW], out[ROW * 2], back[ROW];
    for (unsigned shape = 0; shape < 8; shape++) {
        for (size_t n = 1; n <= ROW; n++) {
            for (size_t i = 0; i < n; i++) {
                switch (shape) {
                case 0: row[i] = (uint8_t)(i * 7 + 1); break;           // all different
                case 1: row[i] = (i & 1) ? 0x00 : 0x12; break;          // alternating
                case 2: row[i] = (uint8_t)(1 + (i / 2) % 200); break;   // pairs (not 00/FF)
                case 3: row[i] = (uint8_t)(1 + (i / 3) % 200); break;   // triples
                case 4: row[i] = (i % 3 == 2) ? 0x00 : 0x80; break;     // single 00 between
                case 5: row[i] = (i / 2 & 1) ? 0x00 : 0xFF; break;      // pairs of 00 and FF
                case 6: row[i] = (uint8_t)(1 + (i / 4) % 200); break;   // runs of 4
                default: row[i] = (uint8_t)((i % 5 == 0) ? 0x00 : (i * 11 + 3)); break;
                }
            }
            size_t len = code(row, NULL, n, out);
            CHECK(len <= sw_code_max(n));
            CHECK_EQ_INT(walk(out, len, n), n);
            CHECK_EQ_INT(sw_decode_row(out, len, NULL, n, back), len);
            CHECK_EQ_MEM(back, row, n);
        }
    }
    // The all-literal row meets the bound exactly: a header per 62 bytes.
    for (size_t i = 0; i < ROW; i++) row[i] = (uint8_t)(i * 7 + 1);
    CHECK_EQ_INT(code(row, NULL, ROW, out), ROW + 6);
    CHECK_EQ_INT(sw_code_max(ROW), ROW + 6);
}

static void test_decode_bad(void)
{
    uint8_t row[16], prev[16];
    memset(prev, 0x3C, sizeof prev);

    // Truncated: a literal that runs out, a repeat missing its byte, a row
    // that is not yet full.
    static const uint8_t trunc_lit[] = { 0x05, 1, 2, 3 };
    CHECK_EQ_INT(sw_decode_row(trunc_lit, sizeof trunc_lit, NULL, 5, row), 0);
    static const uint8_t trunc_rep[] = { 0x45 };
    CHECK_EQ_INT(sw_decode_row(trunc_rep, sizeof trunc_rep, NULL, 5, row), 0);
    static const uint8_t short_row[] = { 0x83, 0xC2 };      // 5 bytes made of 8
    CHECK_EQ_INT(sw_decode_row(short_row, sizeof short_row, NULL, 8, row), 0);
    CHECK_EQ_INT(sw_decode_row(short_row, 1, NULL, 8, row), 0);
    CHECK_EQ_INT(sw_decode_row(short_row, 0, NULL, 8, row), 0);

    // Overrun: the codes make more than the row.
    static const uint8_t over[] = { 0x86, 0xC5 };           // 11 bytes in a row of 8
    CHECK_EQ_INT(sw_decode_row(over, sizeof over, NULL, 8, row), 0);
    static const uint8_t over_lit[] = { 0x04, 1, 2, 3, 4 };
    CHECK_EQ_INT(sw_decode_row(over_lit, sizeof over_lit, NULL, 3, row), 0);
    static const uint8_t over_rep[] = { 0x44, 9 };
    CHECK_EQ_INT(sw_decode_row(over_rep, sizeof over_rep, NULL, 3, row), 0);

    // A zero count anywhere but the lone 80, and a count of 63 or 64.
    static const uint8_t zero_lit[] = { 0x00, 0x85 };
    CHECK_EQ_INT(sw_decode_row(zero_lit, sizeof zero_lit, NULL, 5, row), 0);
    static const uint8_t zero_mid[] = { 0x82, 0x80, 0x83 };
    CHECK_EQ_INT(sw_decode_row(zero_mid, sizeof zero_mid, NULL, 5, row), 0);
    static const uint8_t zero_blk[] = { 0xC0, 0x85 };
    CHECK_EQ_INT(sw_decode_row(zero_blk, sizeof zero_blk, NULL, 5, row), 0);
    static const uint8_t zero_rep[] = { 0x40, 0x12, 0x85 };
    CHECK_EQ_INT(sw_decode_row(zero_rep, sizeof zero_rep, NULL, 5, row), 0);
    static const uint8_t c63[] = { 0xBF };                  // white run of 63
    CHECK_EQ_INT(sw_decode_row(c63, sizeof c63, NULL, 63, row), 0);
    static uint8_t big[70];
    CHECK_EQ_INT(sw_decode_row((const uint8_t[]){ 0xFF }, 1, NULL, 63, big), 0);

    // A row of no bytes has no codes.
    CHECK_EQ_INT(sw_decode_row(c63, sizeof c63, NULL, 0, row), 0);

    // The lone 80 is the row before unchanged, or white with no prev.
    static const uint8_t lone[] = { 0x80, 0x99 };
    memset(row, 0x77, sizeof row);
    CHECK_EQ_INT(sw_decode_row(lone, sizeof lone, NULL, 16, row), 1);
    for (int i = 0; i < 16; i++) CHECK_EQ_INT(row[i], 0);
    CHECK_EQ_INT(sw_decode_row(lone, sizeof lone, prev, 16, row), 1);
    CHECK_EQ_MEM(row, prev, 16);

    // And a good row decodes where the same row's bad cousin did not.
    static const uint8_t ok[] = { 0x83, 0xC5 };             // 3 white, 5 black
    CHECK_EQ_INT(sw_decode_row(ok, sizeof ok, NULL, 8, row), 2);
    static const uint8_t ok_want[] = { 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK_EQ_MEM(row, ok_want, 8);
}

int main(void)
{
    test_known_rows();
    test_round_trip();
    test_worst_case();
    test_decode_bad();
    TEST_END;
}
