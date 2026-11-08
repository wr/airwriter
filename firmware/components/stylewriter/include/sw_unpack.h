// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bytes out of a zlib stream held in flash, in order, a row at a time: the test page's stored
// pictures (sw_scene.h, W-1133). The board inflates with the tinfl in the S3's ROM, the host
// with zlib; both read the same streams. Its memory, about 44 KB on the board (tinfl's state
// and its 32 KB window), comes through bigmem.h.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct sw_unpack sw_unpack_t;

// A reader over the n bytes at z, which must stay put until sw_unpack_free. NULL: no memory.
sw_unpack_t *sw_unpack_new(const uint8_t *z, size_t n);

// The stream's next len bytes into out: 0, or -1 when the stream is bad or ends short.
int sw_unpack_read(sw_unpack_t *u, uint8_t *out, size_t len);

// Takes NULL.
void sw_unpack_free(sw_unpack_t *u);
