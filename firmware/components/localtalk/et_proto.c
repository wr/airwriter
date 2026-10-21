// SPDX-License-Identifier: GPL-3.0-or-later
// EtherTalk packet layouts; see et_proto.h.
#include "et_proto.h"
#include <string.h>

const uint8_t ET_BCAST[ETH_ALEN]    = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const uint8_t ET_AT_MCAST[ETH_ALEN] = { 0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF };

static const uint8_t SNAP_DDP[8]  = { 0xAA, 0xAA, 0x03, 0x08, 0x00, 0x07, 0x80, 0x9B };
static const uint8_t SNAP_AARP[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x80, 0xF3 };
#define SNAP_LEN 8

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

size_t elap_build(uint8_t *out, size_t cap, const uint8_t dst[ETH_ALEN],
                  const uint8_t src[ETH_ALEN], int proto, const uint8_t *data, size_t len)
{
    if (len > DDP_LONG_HDR + DDP_MAX_DATA) return 0;
    size_t n = ELAP_HDR + len;
    size_t total = n < ELAP_MIN_FRAME ? ELAP_MIN_FRAME : n;
    if (total > cap) return 0;
    memcpy(out, dst, ETH_ALEN);
    memcpy(out + 6, src, ETH_ALEN);
    put16(out + 12, (uint16_t)(SNAP_LEN + len));
    memcpy(out + 14, proto == ET_AARP ? SNAP_AARP : SNAP_DDP, SNAP_LEN);
    if (len) memcpy(out + ELAP_HDR, data, len);
    if (total > n) memset(out + n, 0, total - n);
    return total;
}

int elap_parse(const uint8_t *f, size_t n, elap_t *e)
{
    if (n < ELAP_HDR) return -1;
    size_t field = be16(f + 12);
    // 1500 and under is an 802.3 length; above it, an Ethernet II type.
    if (field > 1500 || field < SNAP_LEN || field > n - 14) return -1;
    if (n > ELAP_MAX_FRAME) return -1;
    int proto;
    if (!memcmp(f + 14, SNAP_DDP, SNAP_LEN)) proto = ET_DDP;
    else if (!memcmp(f + 14, SNAP_AARP, SNAP_LEN)) proto = ET_AARP;
    else return -1;
    e->dst = f;
    e->src = f + 6;
    e->proto = proto;
    e->data = f + ELAP_HDR;
    e->len = field - SNAP_LEN;
    return 0;
}

static void put_addr(uint8_t *p, uint16_t net, uint8_t node)
{
    p[0] = 0;
    put16(p + 1, net);
    p[3] = node;
}

int aarp_parse(const uint8_t *p, size_t n, aarp_t *a)
{
    if (n < AARP_LEN) return -1;
    if (be16(p) != 1 || be16(p + 2) != 0x809B || p[4] != 6 || p[5] != 4) return -1;
    uint16_t fn = be16(p + 6);
    if (fn < AARP_REQUEST || fn > AARP_PROBE) return -1;
    a->fn = fn;
    memcpy(a->src_hw, p + 8, ETH_ALEN);
    a->src_net = be16(p + 15);
    a->src_node = p[17];
    memcpy(a->dst_hw, p + 18, ETH_ALEN);
    a->dst_net = be16(p + 25);
    a->dst_node = p[27];
    return 0;
}

size_t aarp_build(uint8_t out[AARP_LEN], const aarp_t *a)
{
    put16(out, 1);
    put16(out + 2, 0x809B);
    out[4] = 6;
    out[5] = 4;
    put16(out + 6, a->fn);
    memcpy(out + 8, a->src_hw, ETH_ALEN);
    put_addr(out + 14, a->src_net, a->src_node);
    memcpy(out + 18, a->dst_hw, ETH_ALEN);
    put_addr(out + 24, a->dst_net, a->dst_node);
    return AARP_LEN;
}
