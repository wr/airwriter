// SPDX-License-Identifier: BSD-2-Clause
//
// lpstyl's scanline coder, for the host tests only (W-1103): a second coder,
// written by someone else against real StyleWriters, to hold ours to. See
// lpstyl_encode.c for where it comes from and its licence.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Codes one scanline of `srcLen` bytes into `dst` (lpstyl's encodescanline). A run of
// white at the end is left to the padding, which writes white to `padTo` bytes.
// lpstyl takes padTo from its PRINT_ROWBYTES constant; here it is a parameter, and
// the tests give it srcLen. Returns the bytes written.
size_t lpstyl_encodescanline(const uint8_t *src, size_t srcLen, size_t padTo, uint8_t *dst);

// Codes the scanline `in` against `last` (XORed with it first), or on its own when
// `last` is NULL (lpstyl's appendEncode). length is at most 1024.
size_t lpstyl_appendEncode(size_t length, const uint8_t *in, const uint8_t *last, uint8_t *out);
