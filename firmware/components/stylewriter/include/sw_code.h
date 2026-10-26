// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter's row run coder (W-1103). A page goes out as bands of rows,
// one plane (an ink) at a time; each plane row is XORed with the same plane of
// the row before in its band (the band's first row raw, and the original
// StyleWriter takes no delta), then coded:
//   low 6 bits  a count, 1 to 62
//   00 literal  that many bytes follow
//   01 repeat   the one byte after it, that many times
//   10 white    that many 00 bytes
//   11 black    that many FF bytes
// 80 alone is a row whose plane is all white after the XOR. Pure C.
#pragma once
#include <stdint.h>
#include <stddef.h>

// The most bytes sw_code_row writes for a row of `rowbytes`: every byte a
// literal, a header per 62 of them.
size_t sw_code_max(size_t rowbytes);

// Codes one plane row of `rowbytes` (at least 1) into `out`, which holds
// sw_code_max(rowbytes). `prev` is the same plane of the row before, or NULL
// for a raw row (a band's first). The XOR is taken as the row is coded, with
// no scratch. Returns the bytes written (1 for the lone 80), 0 for no bytes.
size_t sw_code_row(const uint8_t *row, const uint8_t *prev, size_t rowbytes, uint8_t *out);

// The inverse, for the host tests and the software printer: `in` holds codes
// for one row, and `row` (not overlapping `prev`) gets `rowbytes`. Returns the
// bytes of `in` it used, or 0 for a row that is bad: input that ends early,
// codes that overrun the row, a count of 0 (but for the lone 80) or over 62.
size_t sw_decode_row(const uint8_t *in, size_t n, const uint8_t *prev, size_t rowbytes, uint8_t *row);
