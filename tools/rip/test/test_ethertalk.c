// SPDX-License-Identifier: GPL-3.0-or-later
// EtherTalk (W-1076): Phase 2 framing and AARP, the link that puts the
// LocalTalk node on Ethernet, and the two transports' rules, driven with a
// fake clock and a fake wire that records what goes out.
#include "test_util.h"
#include "lt_proto.h"
#include "et_proto.h"
#include "et_link.h"
#include "lt_node.h"
#include "et_transport.h"

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

// ------------------------------------------------------------- the link
// A fake wire per link (ctx is the link's number) and one fake printer that
// two links share, held by whichever began a job.

#define MAXF 512
static uint8_t  fr[MAXF][ELAP_MAX_FRAME];
static size_t   fr_len[MAXF];
static int      fr_who[MAXF];
static int      nfr;
static char     lastlog[160];
static uint8_t  job[16384];
static size_t   job_len;
static int      job_eof, job_normal = -1, held_by;
static int      claimed_n;
static uint16_t claimed_net;
static uint8_t  claimed_node;
static int      ID_A = 1, ID_B = 2;

static void w_send(void *ctx, const uint8_t *f, size_t n)
{
    if (nfr < MAXF) { memcpy(fr[nfr], f, n); fr_len[nfr] = n; fr_who[nfr] = *(int *)ctx; }
    nfr++;
}
static void w_log(void *ctx, const char *line) { (void)ctx; strncpy(lastlog, line, sizeof lastlog - 1); }
static int w_begin(void *ctx)
{
    int id = *(int *)ctx;
    if (held_by && held_by != id) return 1;
    held_by = id;
    return 0;
}
static void w_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (job_len + n <= sizeof job) { memcpy(job + job_len, d, n); job_len += n; }
}
static size_t w_room(void *ctx) { (void)ctx; return (size_t)-1; }
static void w_end(void *ctx, int normal) { (void)ctx; job_eof++; job_normal = normal; held_by = 0; }
static void w_claimed(void *ctx, uint16_t net, uint8_t node)
{
    (void)ctx;
    claimed_n++; claimed_net = net; claimed_node = node;
}

static void reset_wire(void) { nfr = 0; lastlog[0] = 0; }
static void reset_printer(void) { job_len = 0; job_eof = 0; job_normal = -1; held_by = 0; }

static const uint8_t MAC_A[6]  = { 0x42, 0x32, 0x0A, 0x00, 0x01, 0x73 };
static const uint8_t MAC_B[6]  = { 0x0A, 0x57, 0x57, 0x00, 0x00, 0x01 };
static const uint8_t THEM[6]   = { 0xB2, 0x0A, 0x00, 0x01, 0x6E, 0x00 };   // the Mac
static const uint8_t OTHER[6]  = { 0xB2, 0x0A, 0x00, 0x01, 0x6F, 0x00 };

static lt_reg_t reg_room[4][LT_REG_MAX];
static int reg_next;

static void link_init(et_link_t *l, int *id, const uint8_t mac[6], uint16_t hnet, uint8_t hnode)
{
    et_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, {0}, hnet, hnode, 1, LT_IDLE_HOLD_MS, NULL,
                     reg_room[reg_next++ % 4] };
    memcpy(cfg.mac, mac, 6);
    et_ops_t ops = { id, w_send, w_log, w_begin, w_data, w_room, w_end, w_claimed, NULL, NULL, NULL, NULL };
    et_link_init(l, &cfg, &ops, 0);
}

// Online at hnet.hnode: 10 probes 200 ms apart, ticks every 10 ms.
static uint64_t link_online(et_link_t *l, int *id, const uint8_t mac[6], uint16_t hnet, uint8_t hnode)
{
    link_init(l, id, mac, hnet, hnode);
    uint64_t t = 0;
    for (; t <= 2100; t += 10) et_link_tick(l, t);
    CHECK(l->node.phase == LT_ONLINE);
    reset_wire();
    return t;
}

// A frame from `from` carrying a long DDP datagram from 0xFF10.12:0xFD.
static size_t mac_dg(uint8_t *f, const uint8_t from[6], const uint8_t to_mac[6], uint16_t dnet,
                     uint8_t dnode, uint8_t dsock, uint8_t type, const uint8_t *data, size_t len)
{
    uint8_t ll[LLAP_MAX];
    lt_addr_t to = { dnet, dnode, dsock }, src = { 0xFF10, 12, 0xFD };
    size_t n = ddp_build(ll, sizeof ll, dnode, 12, 1, &to, &src, type, data, len);
    return elap_build(f, ELAP_MAX_FRAME, to_mac, from, ET_DDP, ll + 3, n - 3);
}

static size_t mac_atp(uint8_t *f, uint16_t dnet, uint8_t dnode, uint8_t dsock, uint8_t ctl,
                      uint8_t bitmap, uint16_t tid, uint8_t u0, uint8_t u1,
                      const uint8_t *data, size_t len)
{
    uint8_t a[ATP_HDR + ATP_MAX_DATA];
    uint8_t user[4] = { u0, u1, 0, 0 };
    size_t al = atp_build(a, sizeof a, ctl, bitmap, tid, user, data, len);
    return mac_dg(f, THEM, MAC_A, dnet, dnode, dsock, DDP_ATP, a, al);
}

static size_t aarp_frame(uint8_t *f, const uint8_t from[6], const uint8_t to[6], uint16_t fn,
                         uint16_t snet, uint8_t snode, uint16_t dnet, uint8_t dnode)
{
    aarp_t a = { .fn = fn, .src_net = snet, .src_node = snode, .dst_net = dnet, .dst_node = dnode };
    memcpy(a.src_hw, from, 6);
    uint8_t p[AARP_LEN];
    aarp_build(p, &a);
    return elap_build(f, ELAP_MAX_FRAME, to, from, ET_AARP, p, sizeof p);
}

// What frame i carries: AARP (1, *a) or DDP (2, *d), or 0.
static uint8_t dg_buf[3 + DDP_LONG_HDR + DDP_MAX_DATA];
static int sent_what(int i, aarp_t *a, ddp_t *d)
{
    aarp_t a_;
    ddp_t d_;
    if (!a) a = &a_;
    if (!d) d = &d_;
    elap_t e;
    if (i >= nfr || i >= MAXF || elap_parse(fr[i], fr_len[i], &e)) return 0;
    if (e.proto == ET_AARP) return aarp_parse(e.data, e.len, a) ? 0 : 1;
    dg_buf[0] = e.data[8]; dg_buf[1] = e.data[9]; dg_buf[2] = LLAP_LONG;
    memcpy(dg_buf + 3, e.data, e.len);
    return ddp_parse(dg_buf, 3 + e.len, d) ? 0 : 2;
}

static void test_acquire_clean(void)
{
    et_link_t l;
    reset_wire();
    claimed_n = 0;
    link_init(&l, &ID_A, MAC_A, 0, 0);
    for (uint64_t t = 0; t <= 2500; t += 10) et_link_tick(&l, t);
    CHECK_EQ_INT(nfr, 10);
    uint16_t net = 0; uint8_t node = 0;
    for (int i = 0; i < nfr && i < MAXF; i++) {
        aarp_t a;
        CHECK_EQ_INT(sent_what(i, &a, NULL), 1);
        CHECK_EQ_MEM(fr[i], ET_AT_MCAST, 6);
        CHECK_EQ_INT(a.fn, AARP_PROBE);
        CHECK_EQ_MEM(a.src_hw, MAC_A, 6);
        if (!i) { net = a.dst_net; node = a.dst_node; }
        CHECK_EQ_INT(a.dst_net, net);
        CHECK_EQ_INT(a.dst_node, node);
    }
    CHECK(net >= 0xFF00 && net <= 0xFFFE);
    CHECK(node >= 1 && node <= 253);
    CHECK_EQ_INT(claimed_n, 1);
    CHECK_EQ_INT(claimed_net, net);
    CHECK_EQ_INT(claimed_node, node);
    CHECK(l.node.phase == LT_ONLINE);
    CHECK_EQ_INT(l.node.net, net);
    CHECK_EQ_INT(l.node.node, node);
}

static void test_acquire_hint_and_clash(void)
{
    et_link_t l;
    reset_wire();
    claimed_n = 0;
    link_init(&l, &ID_A, MAC_A, 0xFF23, 77);
    et_link_tick(&l, 0);
    aarp_t a;
    CHECK_EQ_INT(sent_what(0, &a, NULL), 1);
    CHECK_EQ_INT(a.dst_net, 0xFF23);
    CHECK_EQ_INT(a.dst_node, 77);
    uint8_t f[ELAP_MAX_FRAME];
    size_t fl = aarp_frame(f, THEM, MAC_A, AARP_RESPONSE, 0xFF23, 77, 0xFF23, 77);
    et_link_rx(&l, f, fl, 100);
    reset_wire();
    for (uint64_t t = 200; t <= 5000; t += 10) et_link_tick(&l, t);
    CHECK(nfr >= 10);
    CHECK_EQ_INT(sent_what(0, &a, NULL), 1);
    CHECK(!(a.dst_net == 0xFF23 && a.dst_node == 77));
    CHECK_EQ_INT(claimed_n, 1);
    CHECK(!(claimed_net == 0xFF23 && claimed_node == 77));
}

static void test_answers_aarp(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    uint8_t f[ELAP_MAX_FRAME];
    size_t fl = aarp_frame(f, THEM, ET_AT_MCAST, AARP_REQUEST, 0xFF10, 12, 0xFF23, 77);
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(nfr, 1);
    aarp_t a;
    CHECK_EQ_INT(sent_what(0, &a, NULL), 1);
    CHECK_EQ_MEM(fr[0], THEM, 6);
    CHECK_EQ_INT(a.fn, AARP_RESPONSE);
    CHECK_EQ_MEM(a.src_hw, MAC_A, 6);
    CHECK_EQ_INT(a.src_net, 0xFF23);
    CHECK_EQ_INT(a.src_node, 77);
    CHECK_EQ_MEM(a.dst_hw, THEM, 6);
    CHECK_EQ_INT(a.dst_net, 0xFF10);
    CHECK_EQ_INT(a.dst_node, 12);
    fl = aarp_frame(f, THEM, ET_AT_MCAST, AARP_REQUEST, 0xFF10, 12, 0xFF23, 78);
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(nfr, 1);
}

static size_t lkup(uint8_t *nbp)
{
    size_t k = 0;
    nbp[k++] = 0x21; nbp[k++] = 0x1E;
    nbp[k++] = 0xFF; nbp[k++] = 0x10; nbp[k++] = 12; nbp[k++] = 0xFD; nbp[k++] = 0;
    nbp[k++] = 1; nbp[k++] = '=';
    nbp[k++] = 11; memcpy(nbp + k, "ImageWriter", 11); k += 11;
    nbp[k++] = 1; nbp[k++] = '*';
    return k;
}

static void test_lookup_from_mac(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    uint8_t nbp[64], f[ELAP_MAX_FRAME];
    size_t fl = mac_dg(f, THEM, ET_AT_MCAST, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, lkup(nbp));
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(nfr, 1);                                 // no AARP: the sender was gleaned
    ddp_t d;
    CHECK_EQ_INT(sent_what(0, NULL, &d), 2);
    CHECK_EQ_MEM(fr[0], THEM, 6);
    CHECK_EQ_MEM(fr[0] + 6, MAC_A, 6);
    CHECK_EQ_INT(d.dst.net, 0xFF10);
    CHECK_EQ_INT(d.dst.node, 12);
    CHECK_EQ_INT(d.dst.sock, 0xFD);
    CHECK_EQ_INT(d.type, DDP_NBP);
    nbp_t r;
    CHECK_EQ_INT(nbp_parse(d.data, d.len, &r), 0);
    CHECK_EQ_INT(r.fn, NBP_LKUP_REPLY);
    CHECK_EQ_INT(r.t.obj[0], 14);
    CHECK_EQ_INT(memcmp(r.t.obj + 1, "ImageWriter II", 14), 0);
}

// An NBP LkUp of =:type@* from the Mac, as lkup() builds it.
static size_t lkup_type(uint8_t *nbp, const char *type)
{
    size_t k = 0, tl = strlen(type);
    nbp[k++] = 0x21; nbp[k++] = 0x1F;
    nbp[k++] = 0xFF; nbp[k++] = 0x10; nbp[k++] = 12; nbp[k++] = 0xFD; nbp[k++] = 0;
    nbp[k++] = 1; nbp[k++] = '=';
    nbp[k++] = (uint8_t)tl; memcpy(nbp + k, type, tl); k += tl;
    nbp[k++] = 1; nbp[k++] = '*';
    return k;
}

// The replies to one lookup: how many, and the k-th's name and socket.
static int lkup_replies(et_link_t *l, const char *type, uint64_t t, int k, uint8_t *obj, uint8_t *sock)
{
    uint8_t nbp[64], f[ELAP_MAX_FRAME];
    reset_wire();
    size_t fl = mac_dg(f, THEM, ET_AT_MCAST, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, lkup_type(nbp, type));
    et_link_rx(l, f, fl, t);
    int n = 0;
    for (int i = 0; i < nfr; i++) {
        ddp_t d;
        nbp_t r;
        if (sent_what(i, NULL, &d) != 2 || nbp_parse(d.data, d.len, &r) || r.fn != NBP_LKUP_REPLY)
            continue;
        if (n == k) {
            memcpy(obj, r.t.obj, 1 + (size_t)r.t.obj[0]);
            *sock = r.t.addr.sock;
        }
        n++;
    }
    return n;
}

// W-1088: a link offers old Macs the port's ImageWriter under its name, the
// printers relayed from other links under theirs, and the AirWriter marker.
static void test_offers_registered(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    uint8_t own[NBP_NAME_MAX + 1], mark[NBP_NAME_MAX + 1], obj[NBP_NAME_MAX + 1], sock = 0;
    lt_offer_t rel[1];
    nbp_name(own, "Den ImageWriter");
    nbp_name(rel[0].obj, "Office");
    nbp_name(rel[0].type, "LaserWriter");
    nbp_name(mark, "airwriter-2434");
    lt_node_offer(&l.node, own, rel, 1, mark);

    CHECK_EQ_INT(lkup_replies(&l, "LaserWriter", t, 0, obj, &sock), 1);
    CHECK_EQ_MEM(obj, "\x06" "Office", 7);
    CHECK_EQ_INT(sock, 0x85);
    CHECK_EQ_INT(lkup_replies(&l, "ImageWriter", t, 0, obj, &sock), 1);
    CHECK_EQ_MEM(obj, "\x0F" "Den ImageWriter", 16);
    CHECK_EQ_INT(sock, LT_SOCK_SLS);
    CHECK_EQ_INT(lkup_replies(&l, "AirWriter", t, 0, obj, &sock), 1);
    CHECK_EQ_MEM(obj, "\x0E" "airwriter-2434", 15);
    CHECK_EQ_INT(sock, 0x88);

    // No ImageWriter on the port and nothing relayed: only the marker answers.
    lt_node_offer(&l.node, NULL, NULL, 0, mark);
    CHECK_EQ_INT(lkup_replies(&l, "ImageWriter", t, 0, obj, &sock), 0);
    CHECK_EQ_INT(lkup_replies(&l, "LaserWriter", t, 0, obj, &sock), 0);
    CHECK_EQ_INT(lkup_replies(&l, "AirWriter", t, 0, obj, &sock), 1);
}

static void test_unresolved_then_aarp(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    lt_node_echo(&l.node, 99, t);
    CHECK_EQ_INT(nfr, 1);
    aarp_t a;
    CHECK_EQ_INT(sent_what(0, &a, NULL), 1);
    CHECK_EQ_MEM(fr[0], ET_AT_MCAST, 6);
    CHECK_EQ_INT(a.fn, AARP_REQUEST);
    CHECK_EQ_INT(a.dst_net, 0xFF23);
    CHECK_EQ_INT(a.dst_node, 99);
    CHECK_EQ_INT(a.src_net, 0xFF23);
    CHECK_EQ_INT(a.src_node, 77);
    uint8_t f[ELAP_MAX_FRAME];
    size_t fl = aarp_frame(f, OTHER, MAC_A, AARP_RESPONSE, 0xFF23, 99, 0xFF23, 77);
    et_link_rx(&l, f, fl, t + 50);
    CHECK_EQ_INT(nfr, 2);
    ddp_t d;
    CHECK_EQ_INT(sent_what(1, NULL, &d), 2);
    CHECK_EQ_MEM(fr[1], OTHER, 6);
    CHECK_EQ_INT(d.type, DDP_AEP);
    CHECK_EQ_INT(d.dst.node, 99);

    // One nobody answers is asked three times, then dropped.
    reset_wire();
    lt_node_echo(&l.node, 98, t + 100);
    for (uint64_t now = t + 100; now <= t + 3000; now += 10) et_link_tick(&l, now);
    CHECK_EQ_INT(nfr, AARP_REQ_TRIES);
    CHECK_EQ_INT(l.pend_len, 0);
}

static void test_rx_drops(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    uint8_t nbp[64], f[ELAP_MAX_FRAME];
    size_t k = lkup(nbp);
    uint32_t frames = l.node.st.frames;
    size_t fl = mac_dg(f, MAC_A, ET_AT_MCAST, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);   // our own
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(l.st.own, 1);
    fl = mac_dg(f, THEM, OTHER, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);                // not to us
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(l.node.st.frames, frames);
    CHECK_EQ_INT(nfr, 0);
    uint8_t p[20] = {0};
    fl = elap_build(f, sizeof f, ET_AT_MCAST, THEM, ET_AARP, p, sizeof p);         // short AARP
    uint32_t bad = l.st.bad;
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(l.st.bad, bad + 1);
    f[13] = 0xFF;                                                                   // length lies
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(l.st.bad, bad + 2);
    CHECK_EQ_INT(nfr, 0);
}

// The newest SendData link `who` sent, by TID; 0 when there is none.
static int newest_senddata(int who, uint16_t *tid, uint16_t *seq)
{
    int found = 0;
    for (int i = 0; i < nfr && i < MAXF; i++) {
        ddp_t d;
        if (fr_who[i] != who || sent_what(i, NULL, &d) != 2 || d.type != DDP_ATP) continue;
        atp_t a;
        if (atp_parse(d.data, d.len, &a) || (a.ctl & ATP_FN_MASK) != ATP_TREQ || a.user[1] != PAP_SEND_DATA)
            continue;
        *tid = a.tid;
        *seq = (uint16_t)(a.user[2] << 8 | a.user[3]);
        found = 1;
    }
    return found;
}

// OpenConnReply's result from link `who`: 0x0000 or 0xFFFF, or -1 when none.
static int open_result(int who)
{
    for (int i = 0; i < nfr && i < MAXF; i++) {
        ddp_t d;
        atp_t a;
        if (fr_who[i] != who || sent_what(i, NULL, &d) != 2 || d.type != DDP_ATP) continue;
        if (atp_parse(d.data, d.len, &a) || a.user[1] != PAP_OPEN_CONN_REPLY || a.len < 4) continue;
        return a.data[2] << 8 | a.data[3];
    }
    return -1;
}

static void test_pap_fixture_ethertalk(void)
{
    static uint8_t page[16384];
    FILE *fp = fopen("../../fixtures/pap-iw701-page.bin", "rb");
    CHECK(fp != NULL);
    size_t len = fp ? fread(page, 1, sizeof page, fp) : 0;
    if (fp) fclose(fp);
    CHECK_EQ_INT(len, 11968);

    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    reset_printer();
    uint8_t f[ELAP_MAX_FRAME];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 0xFF23, 77, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0400, 0xAD, PAP_OPEN_CONN, oc, sizeof oc);
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(open_result(ID_A), 0x0000);
    size_t off = 0;
    int reads = 0;
    uint16_t last_seq = 0;
    for (int step = 0; step < 100000 && off < len; step++) {
        t += 10;
        uint16_t tid, seq;
        if (newest_senddata(ID_A, &tid, &seq) && seq != last_seq) {
            CHECK_EQ_INT(seq, last_seq + 1);
            last_seq = seq;
            reset_wire();
            size_t k = len - off < 88 ? len - off : 88;
            fl = mac_atp(f, 0xFF23, 77, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid, 0xAD, PAP_DATA, page + off, k);
            et_link_rx(&l, f, fl, t);
            off += k;
            reads++;
        }
        et_link_tick(&l, t);
    }
    CHECK_EQ_INT(off, len);
    fl = mac_atp(f, 0xFF23, 77, LT_SOCK_CONN, ATP_TREQ, 0x01, 0x0500, 0xAD, PAP_CLOSE_CONN, NULL, 0);
    et_link_rx(&l, f, fl, t + 10);
    CHECK_EQ_INT(reads, 136);
    CHECK_EQ_INT(job_len, len);
    CHECK_EQ_MEM(job, page, len);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(job_normal, 1);
    CHECK(!l.node.pap.open);
}

static void test_link_down_mid_job(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    reset_printer();
    uint8_t f[ELAP_MAX_FRAME];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 0xFF23, 77, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0410, 0xAE, PAP_OPEN_CONN, oc, sizeof oc);
    et_link_rx(&l, f, fl, t);
    uint16_t tid, seq;
    CHECK(newest_senddata(ID_A, &tid, &seq));
    const uint8_t chunk[] = { 0x0D, 0x0A, 0x1B, 0x48 };
    fl = mac_atp(f, 0xFF23, 77, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid, 0xAE, PAP_DATA, chunk, sizeof chunk);
    et_link_rx(&l, f, fl, t + 10);
    CHECK_EQ_INT(job_len, 4);
    et_link_down(&l, t + 20);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(job_normal, 0);
    CHECK(!l.node.pap.open);
}

static void test_two_links_one_printer(void)
{
    et_link_t a, b;
    uint64_t t = link_online(&a, &ID_A, MAC_A, 0xFF23, 77);
    link_online(&b, &ID_B, MAC_B, 0xFF24, 78);
    reset_printer();
    reset_wire();
    uint8_t f[ELAP_MAX_FRAME];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 0xFF23, 77, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0420, 0xA1, PAP_OPEN_CONN, oc, sizeof oc);
    et_link_rx(&a, f, fl, t);
    CHECK_EQ_INT(open_result(ID_A), 0x0000);
    CHECK_EQ_INT(held_by, ID_A);
    // B's Mac, on its own link, finds the printer taken.
    uint8_t g[ELAP_MAX_FRAME];
    uint8_t at[ATP_HDR + ATP_MAX_DATA];
    uint8_t user[4] = { 0xB1, PAP_OPEN_CONN, 0, 0 };
    size_t al = atp_build(at, sizeof at, ATP_TREQ | ATP_XO, 0x01, 0x0430, user, oc, sizeof oc);
    size_t gl = mac_dg(g, OTHER, MAC_B, 0xFF24, 78, LT_SOCK_SLS, DDP_ATP, at, al);
    et_link_rx(&b, g, gl, t + 10);
    CHECK_EQ_INT(open_result(ID_B), 0xFFFF);
    // A's Mac finishes; B's next OpenConn is taken.
    fl = mac_atp(f, 0xFF23, 77, LT_SOCK_CONN, ATP_TREQ, 0x01, 0x0421, 0xA1, PAP_CLOSE_CONN, NULL, 0);
    et_link_rx(&a, f, fl, t + 20);
    CHECK_EQ_INT(held_by, 0);
    reset_wire();
    al = atp_build(at, sizeof at, ATP_TREQ | ATP_XO, 0x01, 0x0431, user, oc, sizeof oc);
    gl = mac_dg(g, OTHER, MAC_B, 0xFF24, 78, LT_SOCK_SLS, DDP_ATP, at, al);
    et_link_rx(&b, g, gl, t + 30);
    CHECK_EQ_INT(open_result(ID_B), 0x0000);
    CHECK_EQ_INT(held_by, ID_B);
}

static void test_set_mac(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    reset_printer();
    uint8_t f[ELAP_MAX_FRAME];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 0xFF23, 77, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0440, 0xA2, PAP_OPEN_CONN, oc, sizeof oc);
    et_link_rx(&l, f, fl, t);
    CHECK(l.node.pap.open);
    const uint8_t newmac[6] = { 0x42, 0x32, 0x0A, 0x00, 0x01, 0x80 };
    et_link_set_mac(&l, newmac, t + 10);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(job_normal, 0);
    CHECK(l.node.phase == LT_ACQUIRING);
    for (int i = 0; i < ET_MAP_SLOTS; i++) CHECK_EQ_INT(l.map[i].node, 0);
    reset_wire();
    et_link_tick(&l, t + 20);
    aarp_t a;
    CHECK_EQ_INT(sent_what(0, &a, NULL), 1);
    CHECK_EQ_INT(a.fn, AARP_PROBE);
    CHECK_EQ_MEM(a.src_hw, newmac, 6);
    CHECK_EQ_MEM(fr[0] + 6, newmac, 6);
}

// ------------------------------------------------------------- transports

static void test_tunnel(void)
{
    uint8_t mac[6];
    et_tunnel_mac(0x0A000173, mac);
    static const uint8_t want[6] = { 0x42, 0x32, 0x0A, 0x00, 0x01, 0x73 };
    CHECK_EQ_MEM(mac, want, 6);
    uint32_t ip = 0;
    CHECK_EQ_INT(et_tunnel_dest(mac, &ip), 1);
    CHECK_EQ_INT(ip, 0x0A000173);
    ip = 0;
    CHECK_EQ_INT(et_tunnel_dest(ET_AT_MCAST, &ip), 1);
    CHECK(ip == 0xFFFFFFFFu);
    ip = 0;
    CHECK_EQ_INT(et_tunnel_dest(ET_BCAST, &ip), 1);
    CHECK(ip == 0xFFFFFFFFu);
    CHECK_EQ_INT(et_tunnel_dest(MAC_B, &ip), 0);
    static const uint8_t zone_mac[6] = { 0xB2, 0xA9, 0xD6, 0x35, 0xFA, 0x4A };
    CHECK_EQ_INT(et_tunnel_dest(zone_mac, &ip), 0);       // 0xB2, not "B2"
}

// A send with no buffer free (ENOMEM: Wi-Fi still holds the last burst) waits
// and tries again; a frame dropped there cost a PAP printer a 2 s retry, ten
// of them a minute on board 1 (W-1086). A real failure is not retried.
static int busy_left, sends, waited_ms;
static int send_busy_then_ok(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx; (void)f; (void)n;
    sends++;
    if (busy_left < 0) return ET_SEND_FAILED;
    if (busy_left > 0) { busy_left--; return ET_SEND_BUSY; }
    return ET_SEND_OK;
}
static void wait_ms(uint32_t ms) { waited_ms += (int)ms; }

static void test_send_patiently(void)
{
    static const uint8_t f[4] = { 1, 2, 3, 4 };
    int retries;
    busy_left = 0; sends = 0; waited_ms = 0;
    CHECK_EQ_INT(et_send_patiently(send_busy_then_ok, NULL, f, sizeof f, wait_ms, &retries), 0);
    CHECK_EQ_INT(sends, 1);
    CHECK_EQ_INT(retries, 0);
    CHECK_EQ_INT(waited_ms, 0);

    busy_left = 3; sends = 0; waited_ms = 0;
    CHECK_EQ_INT(et_send_patiently(send_busy_then_ok, NULL, f, sizeof f, wait_ms, &retries), 0);
    CHECK_EQ_INT(sends, 4);
    CHECK_EQ_INT(retries, 3);
    CHECK_EQ_INT(waited_ms, 3 * ET_SEND_WAIT_MS);

    // Still no buffer after every try: given up, as a dropped frame.
    busy_left = 1000; sends = 0; waited_ms = 0;
    CHECK_EQ_INT(et_send_patiently(send_busy_then_ok, NULL, f, sizeof f, wait_ms, &retries), -1);
    CHECK_EQ_INT(sends, ET_SEND_TRIES);
    CHECK_EQ_INT(waited_ms, (ET_SEND_TRIES - 1) * ET_SEND_WAIT_MS);

    // A real failure (no route, a closed socket) is not a burst: no retry.
    busy_left = -1; sends = 0; waited_ms = 0;
    CHECK_EQ_INT(et_send_patiently(send_busy_then_ok, NULL, f, sizeof f, wait_ms, &retries), -1);
    CHECK_EQ_INT(sends, 1);
    CHECK_EQ_INT(waited_ms, 0);
}

static void test_zone_name_ok(void)
{
    char x32[33], x33[34];
    memset(x32, 'x', 32); x32[32] = 0;
    memset(x33, 'x', 33); x33[33] = 0;
    CHECK_EQ_INT(et_zone_name_ok("wells-printer"), 1);
    CHECK_EQ_INT(et_zone_name_ok("a"), 1);
    CHECK_EQ_INT(et_zone_name_ok("A.b_c-9"), 1);
    CHECK_EQ_INT(et_zone_name_ok(x32), 1);
    CHECK_EQ_INT(et_zone_name_ok(""), 0);
    CHECK_EQ_INT(et_zone_name_ok(x33), 0);
    CHECK_EQ_INT(et_zone_name_ok("has space"), 0);
    CHECK_EQ_INT(et_zone_name_ok("a/b"), 0);
    CHECK_EQ_INT(et_zone_name_ok("a?b"), 0);
    CHECK_EQ_INT(et_zone_name_ok("caf\xc3\xa9"), 0);
    CHECK_EQ_INT(et_zone_name_ok("%2F"), 0);
    CHECK_EQ_INT(et_zone_name_ok(NULL), 0);
}

static void test_zone_json(void)
{
    char out[4096];
    size_t n = et_zone_init_json(out, sizeof out, MAC_B);
    const char *want = "{\"type\":\"init\",\"macAddress\":\"0a:57:57:00:00:01\"}";
    CHECK_EQ_INT(n, strlen(want));
    CHECK(!strcmp(out, want));

    uint8_t f[15];
    for (int i = 0; i < 15; i++) f[i] = (uint8_t)(i * 17);
    memcpy(f, ET_AT_MCAST, 6);
    n = et_zone_send_json(out, sizeof out, f, sizeof f);
    const char *at = "{\"type\":\"send\",\"destination\":\"AT\",\"packetArray\":"
                     "[9,0,7,255,255,255,102,119,136,153,170,187,204,221,238]}";
    CHECK_EQ_INT(n, strlen(at));
    CHECK(!strcmp(out, at));
    memcpy(f, ET_BCAST, 6);
    et_zone_send_json(out, sizeof out, f, sizeof f);
    CHECK(strstr(out, "\"destination\":\"*\"") != NULL);
    static const uint8_t them[6] = { 0xB2, 0xA9, 0xD6, 0x35, 0xFA, 0x4A };
    memcpy(f, them, 6);
    et_zone_send_json(out, sizeof out, f, sizeof f);
    CHECK(strstr(out, "\"destination\":\"b2:a9:d6:35:fa:4a\"") != NULL);
    CHECK_EQ_INT(et_zone_send_json(out, 40, f, sizeof f), 0);
    CHECK_EQ_INT(et_zone_init_json(out, 10, MAC_B), 0);
}

static void test_zone_parse(void)
{
    uint8_t f[8];
    size_t n = 0;
    const char *m = "{\"type\":\"receive\",\"packetArray\":[1,2,255]}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), 1);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_INT(f[0], 1);
    CHECK_EQ_INT(f[2], 255);
    m = "{ \"type\" : \"receive\", \"packetArray\" : [ 4, 5 , 6 ] }";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), 1);
    CHECK_EQ_INT(n, 3);
    CHECK_EQ_INT(f[1], 5);
    m = "{\"packetArray\":[7],\"type\":\"receive\"}";            // either order
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), 1);
    CHECK_EQ_INT(n, 1);
    m = "{\"type\":\"other\"}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), 0);
    m = "{\"type\":\"receive\",\"packetArray\":[1,256]}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), -1);
    m = "{\"type\":\"receive\",\"packetArray\":[1,2";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), -1);
    m = "{\"type\":\"receive\",\"packetArray\":[1,x]}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), -1);
    m = "{\"type\":\"receive\",\"packetArray\":[1,2,3,4,5,6,7,8,9]}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), -1);
    m = "{\"type\":\"receive\"}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), -1);
    m = "{\"type\":\"receive\",\"packetArray\":[-1]}";
    CHECK_EQ_INT(et_zone_parse(m, strlen(m), f, sizeof f, &n), -1);
    // Not NUL-terminated: only len bytes are read.
    char buf[64];
    memcpy(buf, "{\"type\":\"receive\",\"packetArray\":[9]}XXXX", 41);
    CHECK_EQ_INT(et_zone_parse(buf, 37, f, sizeof f, &n), 1);
    CHECK_EQ_INT(et_zone_parse(buf, 30, f, sizeof f, &n), -1);
}

static void test_zone_pong(void)
{
    uint8_t ping[26];
    memcpy(ping, ET_BCAST, 6);
    memcpy(ping + 6, MAC_B, 6);
    ping[12] = 0; ping[13] = 12;
    memcpy(ping + 14, "pingPING", 8);
    ping[22] = 0x68; ping[23] = 0xF5; ping[24] = 0x6B; ping[25] = 0x10;
    uint8_t pong[ET_PONG_LEN];
    static const uint8_t me[6] = { 0x0A, 0x01, 0x02, 0x03, 0x04, 0x05 };
    CHECK_EQ_INT(et_zone_pong(ping, sizeof ping, me, pong), 1);
    CHECK_EQ_MEM(pong, MAC_B, 6);
    CHECK_EQ_MEM(pong + 6, me, 6);
    CHECK_EQ_INT(pong[12], 0);
    CHECK_EQ_INT(pong[13], 13);
    CHECK_EQ_MEM(pong + 14, "pingPONG!", 9);
    CHECK_EQ_MEM(pong + 23, ping + 22, 4);
    ping[14] = 'P';
    CHECK_EQ_INT(et_zone_pong(ping, sizeof ping, me, pong), 0);
    ping[14] = 'p';
    CHECK_EQ_INT(et_zone_pong(ping, 25, me, pong), 0);
    uint8_t zm[6];
    static const uint8_t base[6] = { 0x84, 0xF7, 0x03, 0x12, 0x34, 0x56 };
    et_zone_mac(base, zm);
    // Infinite Mac's Basilisk II sends unicast only to MACs starting B2
    // (ether_js.cpp GENERATED_ETHER_ADDR_PREFIX); anything else it drops.
    static const uint8_t wantzm[6] = { 0xB2, 0xF7, 0x03, 0x12, 0x34, 0x56 };
    CHECK_EQ_MEM(zm, wantzm, 6);
}

// ------------------------------------------------------------- the radio (W-1078)
// Frames off the board's own Wi-Fi. Macs send 802.3 + SNAP; an access point
// may hand AppleTalk over as Ethernet II (0x809B, AARP 0x80F3) instead.

static void test_radio_normalize(void)
{
    uint8_t out[ELAP_MAX_FRAME], in[ELAP_MAX_FRAME];
    uint8_t aarp[AARP_LEN] = {0};
    // 802.3 + SNAP passes through as it is, its length from the 802.3 field.
    size_t n = elap_build(in, sizeof in, ET_AT_MCAST, THEM, ET_AARP, aarp, sizeof aarp);
    CHECK_EQ_INT(et_radio_normalize(in, n, out, sizeof out), n);
    CHECK_EQ_MEM(out, in, n);

    // Ethernet II AARP becomes the same 802.3 + SNAP frame.
    uint8_t e2[14 + AARP_LEN];
    memcpy(e2, ET_AT_MCAST, 6); memcpy(e2 + 6, THEM, 6); e2[12] = 0x80; e2[13] = 0xF3;
    memcpy(e2 + 14, aarp, sizeof aarp);
    size_t m = et_radio_normalize(e2, sizeof e2, out, sizeof out);
    CHECK_EQ_INT(m, n);
    CHECK_EQ_MEM(out, in, n);

    // Ethernet II DDP likewise.
    uint8_t dg[20];
    for (int i = 0; i < 20; i++) dg[i] = (uint8_t)i;
    dg[0] = 0; dg[1] = sizeof dg;                         // its DDP length
    n = elap_build(in, sizeof in, MAC_A, THEM, ET_DDP, dg, sizeof dg);
    memcpy(e2, MAC_A, 6); memcpy(e2 + 6, THEM, 6); e2[12] = 0x80; e2[13] = 0x9B;
    memcpy(e2 + 14, dg, sizeof dg);
    CHECK_EQ_INT(et_radio_normalize(e2, 14 + sizeof dg, out, sizeof out), n);
    CHECK_EQ_MEM(out, in, n);
    // Ethernet padding past the DDP length is dropped, not carried along.
    uint8_t e2p[60] = {0};
    memcpy(e2p, e2, 14 + sizeof dg);
    CHECK_EQ_INT(et_radio_normalize(e2p, sizeof e2p, out, sizeof out), n);
    CHECK_EQ_MEM(out, in, n);

    // The SE/30's Chooser lookup as board 1's Wi-Fi handed it over (BlueSCSI
    // DaynaPORT, 2026-10-20): 802.3 + SNAP with a length field of 0. Its
    // length comes back from the DDP header (36), so 8 + 36.
    static const uint8_t se30[58] = {
        0x09,0x00,0x07,0xff,0xff,0xff, 0x00,0x80,0x19,0x64,0xd3,0x64, 0x00,0x00,
        0xaa,0xaa,0x03,0x08,0x00,0x07,0x80,0x9b,
        0x00,0x24,0x00,0x00,0x00,0x00,0xff,0x00,0xff,0x53,0x02,0xfd,0x02,
        0x21,0x30,0xff,0x00,0x53,0xfd,0x00,0x01,'=',0x0b,'I','m','a','g','e','W','r','i','t','e','r',0x01,'*' };
    m = et_radio_normalize(se30, sizeof se30, out, sizeof out);
    CHECK_EQ_INT(m, ELAP_MIN_FRAME);
    CHECK_EQ_INT(out[12], 0);
    CHECK_EQ_INT(out[13], 8 + 36);
    elap_t e;
    CHECK_EQ_INT(elap_parse(out, m, &e), 0);
    CHECK_EQ_INT(e.proto, ET_DDP);
    CHECK_EQ_INT(e.len, 36);
    CHECK_EQ_MEM(e.data, se30 + 22, 36);
    // A zero-length AARP likewise (28).
    n = elap_build(in, sizeof in, ET_AT_MCAST, THEM, ET_AARP, aarp, sizeof aarp);
    in[12] = 0; in[13] = 0;
    CHECK_EQ_INT(et_radio_normalize(in, n, out, sizeof out), n);
    CHECK_EQ_INT(out[13], 8 + AARP_LEN);
    // A DDP length past the frame is still refused.
    uint8_t bad[58];
    memcpy(bad, se30, sizeof bad);
    bad[23] = 0x60;
    CHECK_EQ_INT(et_radio_normalize(bad, sizeof bad, out, sizeof out), 0);

    // Anything else is not ours: IPv4, ARP, other SNAP, runts.
    uint8_t ip[60] = {0};
    ip[12] = 0x08; ip[13] = 0x00;
    CHECK_EQ_INT(et_radio_normalize(ip, sizeof ip, out, sizeof out), 0);
    ip[13] = 0x06;
    CHECK_EQ_INT(et_radio_normalize(ip, sizeof ip, out, sizeof out), 0);
    n = elap_build(in, sizeof in, ET_AT_MCAST, THEM, ET_AARP, aarp, sizeof aarp);
    in[21] = 0x37;                                        // SNAP type not AppleTalk
    CHECK_EQ_INT(et_radio_normalize(in, n, out, sizeof out), 0);
    CHECK_EQ_INT(et_radio_normalize(e2, 13, out, sizeof out), 0);
    // An Ethernet II frame too big to become 802.3 + SNAP is dropped.
    static uint8_t big[14 + DDP_LONG_HDR + DDP_MAX_DATA + 1];
    memcpy(big, e2, 14);
    CHECK_EQ_INT(et_radio_normalize(big, sizeof big, out, sizeof out), 0);
}

// W-1079: the peers a link has heard lately, with the MAC each came from
// (a Basilisk II peer's carries its computer's IP).
static void test_peers(void)
{
    et_link_t l;
    uint64_t t = link_online(&l, &ID_A, MAC_A, 0xFF23, 77);
    uint8_t nbp[64], f[ELAP_MAX_FRAME];
    size_t fl = mac_dg(f, THEM, ET_AT_MCAST, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, lkup(nbp));
    et_link_rx(&l, f, fl, t);                             // from 0xFF10.12, at THEM
    et_peer_t p[ET_MAP_SLOTS];
    size_t k = et_link_peers(&l, t + 3000, 600000, p, ET_MAP_SLOTS);
    CHECK_EQ_INT(k, 1);
    CHECK_EQ_INT(p[0].net, 0xFF10);
    CHECK_EQ_INT(p[0].node, 12);
    CHECK_EQ_MEM(p[0].hw, THEM, 6);
    CHECK_EQ_INT(p[0].ago_s, 3);
    CHECK_EQ_INT(et_link_peers(&l, t + 600001, 600000, p, ET_MAP_SLOTS), 0);
    CHECK_EQ_INT(et_link_peers(&l, t + 3000, 600000, p, 0), 0);

    // The same computer under a new address (an emulator restarted, or a Mac
    // that took another node): one peer, at its newest address.
    fl = aarp_frame(f, THEM, ET_AT_MCAST, AARP_REQUEST, 0xFF10, 99, 0xFF23, 77);
    et_link_rx(&l, f, fl, t + 5000);
    k = et_link_peers(&l, t + 6000, 600000, p, ET_MAP_SLOTS);
    CHECK_EQ_INT(k, 1);
    CHECK_EQ_INT(p[0].node, 99);
    CHECK_EQ_INT(p[0].ago_s, 1);
}

// Macintosh names (as test_localtalk.c has them on the cable): each peer is
// asked for its <name>:Workstation, and the name is kept by its address,
// network and node, since every Mac picks its own startup-range network.
static lt_name_t enames[LT_SEEN_SLOTS];
static const uint8_t SE30[] = { 13, 'W','e','l','l','s',0xD5,'s',' ','S','E','/','3','0' };

static void test_peer_names(void)
{
    et_link_t l;
    et_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, {0}, 0xFF23, 77, 1,
                     LT_IDLE_HOLD_MS, enames, NULL };
    memcpy(cfg.mac, MAC_A, 6);
    et_ops_t ops = { &ID_A, w_send, w_log, w_begin, w_data, w_room, w_end, w_claimed, NULL, NULL, NULL, NULL };
    et_link_init(&l, &cfg, &ops, 0);
    uint64_t t = 0;
    for (; t <= 2100; t += 10) et_link_tick(&l, t);
    CHECK(l.node.phase == LT_ONLINE);
    reset_wire();

    // The Mac at 0xFF10.12 looks the printer up; a moment later it is asked its name.
    uint8_t nbp[2 + 5 + 3 * (NBP_NAME_MAX + 1)], f[ELAP_MAX_FRAME];
    size_t fl = mac_dg(f, THEM, ET_AT_MCAST, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, lkup(nbp));
    et_link_rx(&l, f, fl, t);
    CHECK_EQ_INT(nfr, 1);
    et_link_tick(&l, t + LT_NAME_DELAY_MS - 1);
    CHECK_EQ_INT(nfr, 1);
    et_link_tick(&l, t + LT_NAME_DELAY_MS);
    CHECK_EQ_INT(nfr, 2);
    ddp_t d;
    CHECK_EQ_INT(sent_what(1, NULL, &d), 2);
    CHECK_EQ_MEM(fr[1], THEM, 6);                         // straight to it: no AARP
    CHECK_EQ_INT(d.dst.net, 0xFF10);
    CHECK_EQ_INT(d.dst.node, 12);
    CHECK_EQ_INT(d.dst.sock, SOCK_NIS);
    CHECK_EQ_INT(d.src.sock, SOCK_NIS);
    CHECK_EQ_INT(d.type, DDP_NBP);
    nbp_t q;
    CHECK_EQ_INT(nbp_parse(d.data, d.len, &q), 0);
    CHECK_EQ_INT(q.fn, NBP_LKUP);
    CHECK_EQ_INT(q.t.addr.net, 0xFF23);                   // answers to our NIS
    CHECK_EQ_INT(q.t.addr.node, 77);
    CHECK_EQ_INT(q.t.addr.sock, SOCK_NIS);
    CHECK_EQ_INT(q.t.type[0], 11);
    CHECK_EQ_INT(memcmp(q.t.type + 1, "Workstation", 11), 0);

    // Its answer names it on the peer list.
    nbp_tuple_t r = { .addr = { 0xFF10, 12, SOCK_AEP }, .enumerator = 0 };
    memcpy(r.obj, SE30, sizeof SE30);
    nbp_name(r.type, "Workstation");
    r.zone[0] = 1;
    r.zone[1] = '*';
    size_t k = nbp_build(nbp, sizeof nbp, NBP_LKUP_REPLY, q.id, &r);
    fl = mac_dg(f, THEM, MAC_A, 0xFF23, 77, SOCK_NIS, DDP_NBP, nbp, k);
    et_link_rx(&l, f, fl, t + LT_NAME_DELAY_MS + 30);
    et_peer_t p[ET_MAP_SLOTS];
    k = et_link_peers(&l, t + 2000, 600000, p, ET_MAP_SLOTS);
    CHECK_EQ_INT(k, 1);
    CHECK(p[0].name != NULL);
    if (p[0].name) CHECK_EQ_MEM(p[0].name, SE30, sizeof SE30);

    // Node 12 on another network is another Mac, not yet named.
    fl = aarp_frame(f, OTHER, ET_AT_MCAST, AARP_REQUEST, 0xFF11, 12, 0xFF23, 77);
    et_link_rx(&l, f, fl, t + 3000);
    k = et_link_peers(&l, t + 3000, 600000, p, ET_MAP_SLOTS);
    CHECK_EQ_INT(k, 2);
    CHECK_EQ_INT(p[0].net, 0xFF11);
    CHECK(p[0].name == NULL);
    CHECK_EQ_INT(p[1].net, 0xFF10);
    CHECK(p[1].name != NULL);
}


// W-1082: a lookup of every LaserWriter goes to the AppleTalk multicast.
static void test_lookup_out_elap(void)
{
    et_link_t l;
    int id;
    uint64_t t = link_online(&l, &id, MAC_A, 0, 0);
    lt_node_lookup(&l.node, "LaserWriter", t);
    CHECK_EQ_INT(nfr, 1);
    CHECK_EQ_MEM(fr[0], ET_AT_MCAST, 6);
    ddp_t d;
    CHECK_EQ_INT(sent_what(0, NULL, &d), 2);
    CHECK(d.is_long);
    CHECK_EQ_INT(d.dst.net, 0);
    CHECK_EQ_INT(d.dst.node, 0xFF);
    CHECK_EQ_INT(d.dst.sock, SOCK_NIS);
    CHECK_EQ_INT(d.type, DDP_NBP);
}

int main(void)
{
    test_lookup_out_elap();
    test_elap_round_trip();
    test_elap_parse_rejects();
    test_aarp();
    test_acquire_clean();
    test_acquire_hint_and_clash();
    test_answers_aarp();
    test_lookup_from_mac();
    test_offers_registered();
    test_unresolved_then_aarp();
    test_rx_drops();
    test_pap_fixture_ethertalk();
    test_link_down_mid_job();
    test_two_links_one_printer();
    test_set_mac();
    test_tunnel();
    test_send_patiently();
    test_zone_name_ok();
    test_zone_json();
    test_zone_parse();
    test_zone_pong();
    test_radio_normalize();
    test_peers();
    test_peer_names();
    TEST_END;
}
