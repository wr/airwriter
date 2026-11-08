// SPDX-License-Identifier: GPL-3.0-or-later
//
// sw_unpack.h. On the board, tinfl from the ROM writes into a 32 KB window that wraps, and a
// read copies out what it made before asking for more, so nothing is read after tinfl has
// written over it. On the host, zlib's inflate writes straight into the caller's row.
#include "sw_unpack.h"
#include "bigmem.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM

#include "miniz.h"

struct sw_unpack {
    tinfl_decompressor d;
    const uint8_t     *in;
    size_t             in_left;
    size_t             ofs;            // where tinfl writes next in the window
    size_t             rd, have;       // made and not yet read: `have` bytes at win[rd]
    int                done;
    uint8_t            win[TINFL_LZ_DICT_SIZE];
};

sw_unpack_t *sw_unpack_new(const uint8_t *z, size_t n)
{
    sw_unpack_t *u = big_calloc(1, sizeof *u);
    if (!u) return NULL;
    tinfl_init(&u->d);
    u->in = z;
    u->in_left = n;
    return u;
}

int sw_unpack_read(sw_unpack_t *u, uint8_t *out, size_t len)
{
    while (len) {
        if (u->have) {
            size_t k = len < u->have ? len : u->have;
            memcpy(out, u->win + u->rd, k);
            out += k;
            len -= k;
            u->rd += k;
            u->have -= k;
            continue;
        }
        if (u->done) return -1;
        // Up to the window's end: what tinfl makes is one run, read before it comes round.
        size_t in_n = u->in_left, out_n = TINFL_LZ_DICT_SIZE - u->ofs;
        tinfl_status st = tinfl_decompress(&u->d, u->in, &in_n, u->win, u->win + u->ofs, &out_n,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);
        u->in += in_n;
        u->in_left -= in_n;
        u->rd = u->ofs;
        u->have = out_n;
        u->ofs = (u->ofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        // As miniz's own loop: anything but "more to come" ends the stream, done or bad, and
        // what it made is still read. The ROM's tinfl (miniz 1.x) reads zeros past the end of
        // its input, so a stream cut short is not caught here as it is on the host; ours are
        // whole constants in the signed image.
        if (st != TINFL_STATUS_HAS_MORE_OUTPUT) u->done = 1;
    }
    return 0;
}

void sw_unpack_free(sw_unpack_t *u)
{
    free(u);
}

#else

#include <zlib.h>

struct sw_unpack {
    z_stream z;
    int      bad;
};

sw_unpack_t *sw_unpack_new(const uint8_t *z, size_t n)
{
    sw_unpack_t *u = big_calloc(1, sizeof *u);
    if (!u) return NULL;
    u->z.next_in = (Bytef *)(uintptr_t)z;
    u->z.avail_in = (uInt)n;
    if (inflateInit(&u->z) != Z_OK) {
        free(u);
        return NULL;
    }
    return u;
}

int sw_unpack_read(sw_unpack_t *u, uint8_t *out, size_t len)
{
    if (u->bad) return -1;
    u->z.next_out = out;
    u->z.avail_out = (uInt)len;
    while (u->z.avail_out) {
        int r = inflate(&u->z, Z_NO_FLUSH);
        if (r == Z_OK) continue;
        if (r == Z_STREAM_END && !u->z.avail_out) break;   // the last row ends the stream
        u->bad = 1;                        // the end before the row was full, or a bad stream
        return -1;
    }
    return 0;
}

void sw_unpack_free(sw_unpack_t *u)
{
    if (!u) return;
    inflateEnd(&u->z);
    free(u);
}

#endif
