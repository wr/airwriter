// SPDX-License-Identifier: GPL-3.0-or-later
// AppleTalk packet layouts; see lt_proto.h.
#include "lt_proto.h"
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// ----------------------------------------------------------------- LLAP

static uint16_t crc_ccitt(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

uint16_t llap_fcs(const uint8_t *p, size_t n)
{
    return (uint16_t)~crc_ccitt(p, n);
}

int llap_fcs_ok(const uint8_t *f, size_t n)
{
    return n >= LLAP_HDR + 2 && crc_ccitt(f, n) == 0xF0B8;
}

// ----------------------------------------------------------------- DDP

uint16_t ddp_cksum(const uint8_t *p, size_t n)
{
    uint16_t ck = 0;
    while (n--) {
        ck = (uint16_t)(ck + *p++);
        ck = (uint16_t)(ck << 1 | ck >> 15);
    }
    return ck ? ck : 0xFFFF;
}

int ddp_parse(const uint8_t *f, size_t n, ddp_t *d)
{
    if (n < LLAP_HDR) return -1;
    memset(d, 0, sizeof *d);
    d->llap_dst = f[0];
    d->llap_src = f[1];
    const uint8_t *h = f + LLAP_HDR;
    size_t avail = n - LLAP_HDR;

    if (f[2] == LLAP_SHORT) {
        if (avail < DDP_SHORT_HDR) return -1;
        size_t len = (size_t)((h[0] & 0x03) << 8 | h[1]);
        if (len < DDP_SHORT_HDR || len > avail) return -1;
        d->dst = (lt_addr_t){ 0, f[0], h[2] };
        d->src = (lt_addr_t){ 0, f[1], h[3] };
        d->type = h[4];
        d->data = h + DDP_SHORT_HDR;
        d->len  = len - DDP_SHORT_HDR;
        return 0;
    }
    if (f[2] == LLAP_LONG) {
        if (avail < DDP_LONG_HDR) return -1;
        size_t len = (size_t)((h[0] & 0x03) << 8 | h[1]);
        if (len < DDP_LONG_HDR || len > avail) return -1;
        uint16_t ck = be16(h + 2);
        if (ck && ddp_cksum(h + 4, len - 4) != ck) return -1;
        d->is_long = 1;
        d->hops = (h[0] >> 2) & 0x0F;
        d->dst = (lt_addr_t){ be16(h + 4), h[8], h[10] };
        d->src = (lt_addr_t){ be16(h + 6), h[9], h[11] };
        d->type = h[12];
        d->data = h + DDP_LONG_HDR;
        d->len  = len - DDP_LONG_HDR;
        return 0;
    }
    return -1;
}

size_t ddp_build(uint8_t *out, size_t cap, uint8_t llap_dst, uint8_t llap_src, int is_long,
                 const lt_addr_t *dst, const lt_addr_t *src, uint8_t type,
                 const uint8_t *data, size_t len)
{
    size_t hdr = is_long ? DDP_LONG_HDR : DDP_SHORT_HDR;
    size_t total = LLAP_HDR + hdr + len;
    if (len > DDP_MAX_DATA || total > cap) return 0;
    uint16_t dlen = (uint16_t)(hdr + len);

    out[0] = llap_dst;
    out[1] = llap_src;
    out[2] = is_long ? LLAP_LONG : LLAP_SHORT;
    uint8_t *h = out + LLAP_HDR;
    h[0] = (uint8_t)(dlen >> 8 & 0x03);
    h[1] = (uint8_t)dlen;
    if (is_long) {
        put16(h + 2, 0);                     // no checksum
        put16(h + 4, dst->net);
        put16(h + 6, src->net);
        h[8]  = dst->node;
        h[9]  = src->node;
        h[10] = dst->sock;
        h[11] = src->sock;
        h[12] = type;
    } else {
        h[2] = dst->sock;
        h[3] = src->sock;
        h[4] = type;
    }
    if (len) memcpy(h + hdr, data, len);
    return total;
}

// ----------------------------------------------------------------- NBP

// Copy a Pascal string from p (within end) into dst; returns the byte after it.
static const uint8_t *pstr(const uint8_t *p, const uint8_t *end, uint8_t *dst)
{
    if (!p || p >= end || p[0] > NBP_NAME_MAX || p + 1 + p[0] > end) return NULL;
    memcpy(dst, p, 1 + (size_t)p[0]);
    return p + 1 + p[0];
}

int nbp_parse(const uint8_t *d, size_t n, nbp_t *out)
{
    memset(out, 0, sizeof *out);
    if (n < 2 + 5) return -1;
    out->fn    = d[0] >> 4;
    out->count = d[0] & 0x0F;
    out->id    = d[1];
    const uint8_t *p = d + 2, *end = d + n;
    out->t.addr = (lt_addr_t){ be16(p), p[2], p[3] };
    out->t.enumerator = p[4];
    p += 5;
    p = pstr(p, end, out->t.obj);
    p = pstr(p, end, out->t.type);
    p = pstr(p, end, out->t.zone);
    return p ? 0 : -1;
}

static uint8_t fold(uint8_t c)
{
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
}

static int same(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (fold(a[i]) != fold(b[i])) return 0;
    return 1;
}

int nbp_match(const uint8_t *pattern, const uint8_t *name)
{
    size_t pl = pattern[0], nl = name[0];
    const uint8_t *p = pattern + 1, *s = name + 1;
    if (pl == 1 && p[0] == NBP_WILD_ALL) return 1;
    const uint8_t *w = memchr(p, NBP_WILD_PART, pl);
    if (!w) return pl == nl && same(p, s, pl);
    size_t head = (size_t)(w - p), tail = pl - head - 1;
    return nl >= head + tail && same(p, s, head) && same(w + 1, s + nl - tail, tail);
}

size_t nbp_build(uint8_t *out, size_t cap, uint8_t fn, uint8_t id, const nbp_tuple_t *t)
{
    size_t n = 2 + 5 + 3 + (size_t)t->obj[0] + t->type[0] + t->zone[0];
    if (n > cap) return 0;
    uint8_t *p = out;
    *p++ = (uint8_t)(fn << 4 | 1);
    *p++ = id;
    put16(p, t->addr.net);
    p[2] = t->addr.node;
    p[3] = t->addr.sock;
    p[4] = t->enumerator;
    p += 5;
    memcpy(p, t->obj, 1 + (size_t)t->obj[0]);   p += 1 + t->obj[0];
    memcpy(p, t->type, 1 + (size_t)t->type[0]); p += 1 + t->type[0];
    memcpy(p, t->zone, 1 + (size_t)t->zone[0]);
    return n;
}

// MacRoman 0x80-0xFF in Unicode, as Apple's ROMAN.TXT (and Python's mac_roman)
// has them: 0xDB is the euro since Mac OS 8.5, the currency sign before.
static const uint16_t mac_roman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

void nbp_name(uint8_t *dst, const char *src)
{
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)src;
    while (*s && n < NBP_NAME_MAX) {
        if (*s < 0x80) {
            dst[1 + n++] = *s++;
            continue;
        }
        // One UTF-8 character: its MacRoman byte, or one '?' for it.
        uint32_t cp = 0;
        int more = *s >= 0xF0 ? 3 : *s >= 0xE0 ? 2 : *s >= 0xC0 ? 1 : 0;
        cp = *s++ & (0x3F >> more);
        for (int i = 0; i < more && (*s & 0xC0) == 0x80; i++) cp = cp << 6 | (*s++ & 0x3F);
        while ((*s & 0xC0) == 0x80) s++;          // a stray continuation byte
        uint8_t b = '?';
        for (int i = 0; i < 128; i++)
            if (mac_roman[i] == cp) { b = (uint8_t)(0x80 + i); break; }
        dst[1 + n++] = b;
    }
    dst[0] = (uint8_t)n;
}

size_t nbp_utf8(char *out, size_t cap, const uint8_t *name)
{
    size_t k = 0;
    for (size_t i = 0; i < name[0]; i++) {
        uint8_t c = name[1 + i];
        uint16_t u = c >= 0x80 ? mac_roman[c - 0x80] : (c < 0x20 || c == 0x7F) ? '?' : c;
        char b[3];
        size_t n = 0;
        if (u < 0x80) {
            b[n++] = (char)u;
        } else if (u < 0x800) {
            b[n++] = (char)(0xC0 | u >> 6);
            b[n++] = (char)(0x80 | (u & 0x3F));
        } else {
            b[n++] = (char)(0xE0 | u >> 12);
            b[n++] = (char)(0x80 | (u >> 6 & 0x3F));
            b[n++] = (char)(0x80 | (u & 0x3F));
        }
        if (k + n >= cap) break;
        memcpy(out + k, b, n);
        k += n;
    }
    if (cap) out[k] = '\0';
    return k;
}

// ----------------------------------------------------------------- ATP

int atp_parse(const uint8_t *d, size_t n, atp_t *a)
{
    if (n < ATP_HDR) return -1;
    a->ctl    = d[0];
    a->bitmap = d[1];
    a->tid    = be16(d + 2);
    memcpy(a->user, d + 4, 4);
    a->data   = d + ATP_HDR;
    a->len    = n - ATP_HDR;
    return 0;
}

size_t atp_build(uint8_t *out, size_t cap, uint8_t ctl, uint8_t bitmap, uint16_t tid,
                 const uint8_t user[4], const uint8_t *data, size_t len)
{
    if (len > ATP_MAX_DATA || ATP_HDR + len > cap) return 0;
    out[0] = ctl;
    out[1] = bitmap;
    put16(out + 2, tid);
    memcpy(out + 4, user, 4);
    if (len) memcpy(out + ATP_HDR, data, len);
    return ATP_HDR + len;
}
