// SPDX-License-Identifier: GPL-3.0-or-later
//
// A minimal reader over an encoded IPP message, for tests only. It walks the
// attribute stream the encoder produced and finds an attribute by name, which
// is how the attribute-set tests assert content without hard-coding offsets
// that shift whenever an attribute is added.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint8_t     tag;
    uint8_t     group;
    const uint8_t *val;
    size_t      vlen;
    int         nvalues;   // this value plus any that follow with empty names
} ipp_found_t;

// Returns 1 and fills `out` when `name` is present. Walks the whole message,
// so a malformed length lands outside the buffer and stops the scan.
static inline int ipp_find(const uint8_t *buf, size_t len,
                           const char *name, ipp_found_t *out)
{
    size_t pos = 8;            // past version, status, request-id
    uint8_t group = 0;
    int hit = 0;

    while (pos < len) {
        uint8_t tag = buf[pos++];
        if (tag == 0x03) break;
        if (tag <= 0x0F) { group = tag; continue; }

        if (pos + 2 > len) break;
        size_t nlen = (size_t)((buf[pos] << 8) | buf[pos + 1]);
        pos += 2;
        if (pos + nlen > len) break;
        const uint8_t *nm = buf + pos;
        pos += nlen;

        if (pos + 2 > len) break;
        size_t vlen = (size_t)((buf[pos] << 8) | buf[pos + 1]);
        pos += 2;
        if (pos + vlen > len) break;
        const uint8_t *val = buf + pos;
        pos += vlen;

        if (hit && nlen == 0) { out->nvalues++; continue; }
        if (hit) break;                       // the attribute ended

        if (nlen == strlen(name) && memcmp(nm, name, nlen) == 0) {
            out->tag = tag;
            out->group = group;
            out->val = val;
            out->vlen = vlen;
            out->nvalues = 1;
            hit = 1;
        }
    }
    return hit;
}

static inline int ipp_val_is(const ipp_found_t *f, const char *s)
{
    return f->vlen == strlen(s) && memcmp(f->val, s, f->vlen) == 0;
}

static inline int32_t ipp_val_int(const ipp_found_t *f)
{
    if (f->vlen != 4) return -1;
    return (int32_t)(((uint32_t)f->val[0] << 24) | ((uint32_t)f->val[1] << 16) |
                     ((uint32_t)f->val[2] << 8)  | (uint32_t)f->val[3]);
}
