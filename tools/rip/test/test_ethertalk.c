// SPDX-License-Identifier: GPL-3.0-or-later
// EtherTalk (W-1076): Phase 2 framing and AARP, the link that puts the
// LocalTalk node on Ethernet, and the two transports' rules, driven with a
// fake clock and a fake wire that records what goes out.
#include "test_util.h"
#include "lt_proto.h"
#include "et_proto.h"

static const uint8_t SNAP_DDP[8]  = { 0xAA, 0xAA, 0x03, 0x08, 0x00, 0x07, 0x80, 0x9B };
static const uint8_t SNAP_AARP[8] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x80, 0xF3 };
static const uint8_t ME[6]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };

// ------------------------------------------------------------- framing

static void test_elap_round_trip(void)
{
    uint8_t ddp[13], f[ELAP_MAX_FRAME];
    for (int i = 0; i < 13; i++) ddp[i] = (uint8_t)(0x10 + i);
    size_t n = elap_build(f, sizeof f, ET_AT_MCAST, ME, ET_DDP, ddp, sizeof ddp);
    CHECK_EQ_INT(n, ELAP_MIN_FRAME);
    CHECK_EQ_MEM(f, ET_AT_MCAST, 6);
    CHECK_EQ_MEM(f + 6, ME, 6);
    CHECK_EQ_INT(f[12], 0x00);
    CHECK_EQ_INT(f[13], 0x15);                       // 8 + 13
    CHECK_EQ_MEM(f + 14, SNAP_DDP, 8);
    CHECK_EQ_MEM(f + ELAP_HDR, ddp, sizeof ddp);
    for (size_t i = ELAP_HDR + sizeof ddp; i < n; i++) CHECK_EQ_INT(f[i], 0);

    elap_t e;
    CHECK_EQ_INT(elap_parse(f, n, &e), 0);
    CHECK_EQ_INT(e.proto, ET_DDP);
    CHECK_EQ_INT(e.len, sizeof ddp);
    CHECK_EQ_MEM(e.data, ddp, sizeof ddp);
    CHECK_EQ_MEM(e.dst, ET_AT_MCAST, 6);
    CHECK_EQ_MEM(e.src, ME, 6);

    uint8_t aarp[AARP_LEN] = {0};
    n = elap_build(f, sizeof f, ET_BCAST, ME, ET_AARP, aarp, sizeof aarp);
    CHECK_EQ_INT(n, ELAP_MIN_FRAME);
    CHECK_EQ_INT(f[13], 8 + AARP_LEN);
    CHECK_EQ_MEM(f + 14, SNAP_AARP, 8);
    CHECK_EQ_INT(elap_parse(f, n, &e), 0);
    CHECK_EQ_INT(e.proto, ET_AARP);
    CHECK_EQ_INT(e.len, AARP_LEN);

    // A big one needs no padding, and one that does not fit is refused.
    static uint8_t big[DDP_LONG_HDR + DDP_MAX_DATA];
    n = elap_build(f, sizeof f, ME, ME, ET_DDP, big, sizeof big);
    CHECK_EQ_INT(n, ELAP_MAX_FRAME);
    CHECK_EQ_INT(elap_build(f, 100, ME, ME, ET_DDP, big, sizeof big), 0);
    CHECK_EQ_INT(elap_build(f, sizeof f, ME, ME, ET_DDP, big, sizeof big + 1), 0);
}

static void test_elap_parse_rejects(void)
{
    uint8_t ddp[20] = {0}, f[ELAP_MAX_FRAME + 8];
    size_t n = elap_build(f, sizeof f, ET_AT_MCAST, ME, ET_DDP, ddp, sizeof ddp);
    elap_t e;
    CHECK_EQ_INT(elap_parse(f, n, &e), 0);

    uint8_t g[sizeof f];
    memcpy(g, f, n); g[12] = 0x08; g[13] = 0x00;          // Ethernet II, IP
    CHECK_EQ_INT(elap_parse(g, n, &e), -1);
    memcpy(g, f, n); g[12] = 0x00; g[13] = (uint8_t)(n - 14 + 1);   // longer than the frame
    CHECK_EQ_INT(elap_parse(g, n, &e), -1);
    memcpy(g, f, n); g[13] = 7;                           // shorter than LLC + SNAP
    CHECK_EQ_INT(elap_parse(g, n, &e), -1);
    CHECK_EQ_INT(elap_parse(f, ELAP_HDR - 1, &e), -1);
    memset(g, 0, sizeof g);
    memcpy(g, f, n);
    CHECK_EQ_INT(elap_parse(g, ELAP_MAX_FRAME + 1, &e), -1);   // over the largest frame
    memcpy(g, f, n); g[21] = 0x9C;                        // SNAP neither DDP nor AARP
    CHECK_EQ_INT(elap_parse(g, n, &e), -1);
    memcpy(g, f, n); g[14] = 0xAB;                        // not SNAP
    CHECK_EQ_INT(elap_parse(g, n, &e), -1);
}

// ------------------------------------------------------------- AARP

static void test_aarp(void)
{
    aarp_t a = { .fn = AARP_PROBE, .src_net = 0xFF23, .src_node = 77,
                 .dst_net = 0xFF23, .dst_node = 77 };
    memcpy(a.src_hw, ME, 6);
    uint8_t p[AARP_LEN];
    CHECK_EQ_INT(aarp_build(p, &a), AARP_LEN);
    static const uint8_t head[8] = { 0x00, 0x01, 0x80, 0x9B, 0x06, 0x04, 0x00, 0x03 };
    CHECK_EQ_MEM(p, head, 8);
    CHECK_EQ_MEM(p + 8, ME, 6);
    static const uint8_t addr[4] = { 0x00, 0xFF, 0x23, 0x4D };
    CHECK_EQ_MEM(p + 14, addr, 4);
    static const uint8_t zero6[6] = {0};
    CHECK_EQ_MEM(p + 18, zero6, 6);
    CHECK_EQ_MEM(p + 24, addr, 4);

    aarp_t b;
    CHECK_EQ_INT(aarp_parse(p, sizeof p, &b), 0);
    CHECK_EQ_INT(b.fn, AARP_PROBE);
    CHECK_EQ_MEM(b.src_hw, ME, 6);
    CHECK_EQ_INT(b.src_net, 0xFF23);
    CHECK_EQ_INT(b.src_node, 77);
    CHECK_EQ_INT(b.dst_net, 0xFF23);
    CHECK_EQ_INT(b.dst_node, 77);

    uint8_t q[AARP_LEN];
    memcpy(q, p, sizeof q); q[1] = 0x06;                  // hardware type 6
    CHECK_EQ_INT(aarp_parse(q, sizeof q, &b), -1);
    memcpy(q, p, sizeof q); q[3] = 0x9C;                  // not AppleTalk
    CHECK_EQ_INT(aarp_parse(q, sizeof q, &b), -1);
    memcpy(q, p, sizeof q); q[4] = 8;
    CHECK_EQ_INT(aarp_parse(q, sizeof q, &b), -1);
    memcpy(q, p, sizeof q); q[5] = 2;
    CHECK_EQ_INT(aarp_parse(q, sizeof q, &b), -1);
    memcpy(q, p, sizeof q); q[7] = 0;
    CHECK_EQ_INT(aarp_parse(q, sizeof q, &b), -1);
    memcpy(q, p, sizeof q); q[7] = 4;
    CHECK_EQ_INT(aarp_parse(q, sizeof q, &b), -1);
    CHECK_EQ_INT(aarp_parse(p, AARP_LEN - 1, &b), -1);
}

int main(void)
{
    test_elap_round_trip();
    test_elap_parse_rejects();
    test_aarp();
    TEST_END;
}
