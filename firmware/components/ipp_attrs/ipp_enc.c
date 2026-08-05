// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipp_enc.h"
#include <string.h>

static void put(ipp_enc_t *e, const void *src, size_t n)
{
    if (e->overflow) return;
    if (e->len + n > e->cap) { e->overflow = 1; return; }
    memcpy(e->buf + e->len, src, n);
    e->len += n;
}

static void put_u8(ipp_enc_t *e, uint8_t v) { put(e, &v, 1); }

static void put_u16(ipp_enc_t *e, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    put(e, b, 2);
}

static void put_u32(ipp_enc_t *e, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    put(e, b, 4);
}

// Name and value lengths are signed 16-bit on the wire, so anything past
// 32767 cannot be expressed. Treat it as overflow rather than truncating:
// a silently short attribute is worse than a failed response.
static void put_len_bytes(ipp_enc_t *e, const void *p, size_t n)
{
    if (n > 32767) { e->overflow = 1; return; }
    put_u16(e, (uint16_t)n);
    put(e, p, n);
}

void ipp_enc_init(ipp_enc_t *e, uint8_t *buf, size_t cap)
{
    e->buf = buf;
    e->cap = cap;
    e->len = 0;
    e->overflow = 0;
}

void ipp_resp_begin(ipp_enc_t *e, uint16_t status, uint32_t request_id)
{
    put_u16(e, IPP_VERSION_1_1);
    put_u16(e, status);
    put_u32(e, request_id);
}

void ipp_group(ipp_enc_t *e, uint8_t delimiter_tag) { put_u8(e, delimiter_tag); }
void ipp_end(ipp_enc_t *e) { put_u8(e, IPP_TAG_END); }

static void attr_head(ipp_enc_t *e, uint8_t tag, const char *name)
{
    put_u8(e, tag);
    put_len_bytes(e, name, strlen(name));
}

void ipp_attr_str(ipp_enc_t *e, uint8_t tag, const char *name, const char *val)
{
    attr_head(e, tag, name);
    put_len_bytes(e, val, strlen(val));
}

void ipp_attr_int(ipp_enc_t *e, uint8_t tag, const char *name, int32_t val)
{
    attr_head(e, tag, name);
    put_u16(e, 4);
    put_u32(e, (uint32_t)val);
}

void ipp_attr_bool(ipp_enc_t *e, const char *name, int val)
{
    attr_head(e, IPP_TAG_BOOLEAN, name);
    put_u16(e, 1);
    put_u8(e, val ? 1 : 0);
}

void ipp_attr_range(ipp_enc_t *e, const char *name, int32_t lo, int32_t hi)
{
    attr_head(e, IPP_TAG_RANGE, name);
    put_u16(e, 8);
    put_u32(e, (uint32_t)lo);
    put_u32(e, (uint32_t)hi);
}

void ipp_attr_res(ipp_enc_t *e, const char *name, int32_t x, int32_t y)
{
    attr_head(e, IPP_TAG_RESOLUTION, name);
    put_u16(e, 9);
    put_u32(e, (uint32_t)x);
    put_u32(e, (uint32_t)y);
    put_u8(e, IPP_RES_DPI);
}

void ipp_addl_str(ipp_enc_t *e, uint8_t tag, const char *val)
{
    put_u8(e, tag);
    put_u16(e, 0);                     // empty name marks an additional value
    put_len_bytes(e, val, strlen(val));
}

void ipp_addl_int(ipp_enc_t *e, uint8_t tag, int32_t val)
{
    put_u8(e, tag);
    put_u16(e, 0);
    put_u16(e, 4);
    put_u32(e, (uint32_t)val);
}

void ipp_attr_strs(ipp_enc_t *e, uint8_t tag, const char *name,
                   const char *const *vals, size_t n)
{
    if (n == 0) return;
    ipp_attr_str(e, tag, name, vals[0]);
    for (size_t i = 1; i < n; i++) ipp_addl_str(e, tag, vals[i]);
}

void ipp_attr_ints(ipp_enc_t *e, uint8_t tag, const char *name,
                   const int32_t *vals, size_t n)
{
    if (n == 0) return;
    ipp_attr_int(e, tag, name, vals[0]);
    for (size_t i = 1; i < n; i++) ipp_addl_int(e, tag, vals[i]);
}
