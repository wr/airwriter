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

static void link_init(et_link_t *l, int *id, const uint8_t mac[6], uint16_t hnet, uint8_t hnode)
{
    et_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, {0}, hnet, hnode, 1, LT_IDLE_HOLD_MS };
    memcpy(cfg.mac, mac, 6);
    et_ops_t ops = { id, w_send, w_log, w_begin, w_data, w_room, w_end, w_claimed };
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
    static const uint8_t wantzm[6] = { 0x0A, 0xF7, 0x03, 0x12, 0x34, 0x56 };
    CHECK_EQ_MEM(zm, wantzm, 6);
}

int main(void)
{
    test_elap_round_trip();
    test_elap_parse_rejects();
    test_aarp();
    test_acquire_clean();
    test_acquire_hint_and_clash();
    test_answers_aarp();
    test_lookup_from_mac();
    test_unresolved_then_aarp();
    test_rx_drops();
    test_pap_fixture_ethertalk();
    test_link_down_mid_job();
    test_two_links_one_printer();
    test_set_mac();
    test_tunnel();
    test_zone_name_ok();
    test_zone_json();
    test_zone_parse();
    test_zone_pong();
    TEST_END;
}
