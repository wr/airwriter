// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_code.h"

#define RUN_MAX 62               // a code's largest count

enum { C_LIT = 0x00, C_REP = 0x40, C_WHITE = 0x80, C_BLACK = 0xC0 };

// The byte at i of the delta row: the row XOR the row before, taken as it is
// wanted so the coder needs no scratch row.
static inline uint8_t dat(const uint8_t *row, const uint8_t *prev, size_t i)
{
    return prev ? (uint8_t)(row[i] ^ prev[i]) : row[i];
}

// How many bytes from i equal dat(i), at most `cap`.
static size_t same(const uint8_t *row, const uint8_t *prev, size_t n, size_t i, size_t cap)
{
    uint8_t b = dat(row, prev, i);
    size_t k = 1;
    while (k < cap && i + k < n && dat(row, prev, i + k) == b) k++;
    return k;
}

// A run of its own begins at i: two or more 00 or FF, or three or more of any
// byte with five or more left in the row (a repeat code is two bytes, so it
// pays from three, and the row's last bytes stay literal).
static int run_at(const uint8_t *row, const uint8_t *prev, size_t n, size_t i)
{
    uint8_t b = dat(row, prev, i);
    if ((b == 0x00 || b == 0xFF) && same(row, prev, n, i, 2) >= 2) return 1;
    return n - i >= 5 && same(row, prev, n, i, 3) >= 3;
}

size_t sw_code_max(size_t rowbytes)
{
    return rowbytes + rowbytes / RUN_MAX + 1;
}

size_t sw_code_row(const uint8_t *row, const uint8_t *prev, size_t rowbytes, uint8_t *out)
{
    if (rowbytes == 0) return 0;
    size_t i = 0;
    while (i < rowbytes && dat(row, prev, i) == 0) i++;
    if (i == rowbytes) { out[0] = C_WHITE; return 1; }

    size_t o = 0;
    for (i = 0; i < rowbytes;) {
        uint8_t b = dat(row, prev, i);
        size_t run = same(row, prev, rowbytes, i, RUN_MAX);
        if ((b == 0x00 || b == 0xFF) && run >= 2) {
            out[o++] = (uint8_t)((b ? C_BLACK : C_WHITE) | run);
            i += run;
        } else if (run >= 3 && rowbytes - i >= 5) {
            out[o++] = (uint8_t)(C_REP | run);
            out[o++] = b;
            i += run;
        } else {
            size_t k = 1;                    // a literal, up to the next run
            while (k < RUN_MAX && i + k < rowbytes && !run_at(row, prev, rowbytes, i + k)) k++;
            out[o++] = (uint8_t)(C_LIT | k);
            for (size_t m = 0; m < k; m++) out[o++] = dat(row, prev, i + m);
            i += k;
        }
    }
    return o;
}

size_t sw_decode_row(const uint8_t *in, size_t n, const uint8_t *prev, size_t rowbytes, uint8_t *row)
{
    if (rowbytes == 0 || n == 0) return 0;
    size_t used = 0, made = 0;
    if (in[0] == C_WHITE) {                  // the lone 80: no change from the row before
        used = 1;
        made = rowbytes;
        for (size_t k = 0; k < rowbytes; k++) row[k] = 0;
    }
    while (made < rowbytes) {
        if (used >= n) return 0;
        uint8_t h = in[used++];
        size_t cnt = h & 63;
        if (cnt < 1 || cnt > RUN_MAX || made + cnt > rowbytes) return 0;
        switch (h & 0xC0) {
        case C_LIT:
            if (used + cnt > n) return 0;
            for (size_t k = 0; k < cnt; k++) row[made + k] = in[used + k];
            used += cnt;
            break;
        case C_REP:
            if (used >= n) return 0;
            for (size_t k = 0; k < cnt; k++) row[made + k] = in[used];
            used++;
            break;
        default:
            for (size_t k = 0; k < cnt; k++) row[made + k] = (h & 0xC0) == C_BLACK ? 0xFF : 0x00;
            break;
        }
        made += cnt;
    }
    if (prev)
        for (size_t k = 0; k < rowbytes; k++) row[k] ^= prev[k];
    return used;
}
