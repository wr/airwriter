// SPDX-License-Identifier: GPL-3.0-or-later
// LocalTalk node (W-1051): packet layouts and the node state machine, driven
// with a fake clock and a fake TashTalk that records what the node sends.
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "lt_tashtalk.h"

// ------------------------------------------------------------- the fake

#define MAXSENT 64
static uint8_t  sent[MAXSENT][LLAP_MAX];
static size_t   sent_len[MAXSENT];
static int      nsent;
static int      claimed = -1;
static char     lastlog[160];
static int      nlogs;
static uint8_t  job[8192];
static size_t   job_len;
static int      job_eof;

static void f_send(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (nsent < MAXSENT) { memcpy(sent[nsent], f, n); sent_len[nsent] = n; }
    nsent++;
}
static void f_claim(void *ctx, uint8_t node) { (void)ctx; claimed = node; }
static void f_log(void *ctx, const char *line)
{
    (void)ctx;
    strncpy(lastlog, line, sizeof lastlog - 1);
    nlogs++;
}
static void f_data(void *ctx, const uint8_t *d, size_t n, int eof)
{
    (void)ctx;
    if (n && job_len + n <= sizeof job) { memcpy(job + job_len, d, n); job_len += n; }
    if (eof) job_eof++;
}

static const lt_ops_t OPS = { NULL, f_send, f_claim, f_log, f_data };

static void reset_fake(void) { nsent = 0; nlogs = 0; lastlog[0] = 0; }

// Bring a node online at `id` with no clash: 120 enquiries 10 ms apart.
static uint64_t online(lt_node_t *n, uint8_t id)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, id, 1 };
    lt_node_init(n, &cfg, &OPS, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(n, t);
    reset_fake();
    return t;
}

// An LLAP + short-DDP frame from the Mac (node 83, socket 0xFB).
static size_t mac_ddp(uint8_t *f, uint8_t dst, uint8_t dsock, uint8_t type,
                      const uint8_t *data, size_t len)
{
    lt_addr_t to = { 0, dst, dsock }, from = { 0, 83, 0xFB };
    return ddp_build(f, LLAP_MAX, dst, 83, 0, &to, &from, type, data, len);
}

static size_t mac_atp(uint8_t *f, uint8_t dst, uint8_t dsock, uint8_t ctl, uint8_t bitmap,
                      uint16_t tid, uint8_t u0, uint8_t u1, uint8_t u2, uint8_t u3,
                      const uint8_t *data, size_t len)
{
    uint8_t a[ATP_HDR + ATP_MAX_DATA];
    uint8_t user[4] = { u0, u1, u2, u3 };
    size_t al = atp_build(a, sizeof a, ctl, bitmap, tid, user, data, len);
    return mac_ddp(f, dst, dsock, DDP_ATP, a, al);
}

// ------------------------------------------------------------- tests

static void test_fcs(void)
{
    // An RTS the Mac sent on the bench (W-1050): ff 53 84, FCS 8c 3b.
    const uint8_t rts[] = { 0xFF, 0x53, 0x84, 0x8C, 0x3B };
    CHECK(llap_fcs_ok(rts, sizeof rts));
    CHECK_EQ_INT(llap_fcs(rts, 3), 0x3B8C);
    uint8_t bad[sizeof rts];
    memcpy(bad, rts, sizeof rts);
    bad[1] ^= 1;
    CHECK(!llap_fcs_ok(bad, sizeof bad));
}

static tt_event_t tt_ev[8];
static uint8_t    tt_f[8][32];
static size_t     tt_n[8];
static int        tt_count;

static void tt_cb(void *ctx, tt_event_t ev, const uint8_t *f, size_t n)
{
    (void)ctx;
    if (tt_count < 8) {
        tt_ev[tt_count] = ev;
        tt_n[tt_count] = n;
        memcpy(tt_f[tt_count], f, n < 32 ? n : 32);
    }
    tt_count++;
}

static void test_tashtalk(void)
{
    tt_rx_t r;
    tt_rx_init(&r);
    // The bench RTS, a frame with a literal zero split across two reads, then
    // a framing error and a bad-CRC report.
    const uint8_t a[] = { 0xFF, 0x53, 0x84, 0x8C, 0x3B, 0x00, 0xFD, 0x01, 0x00 };
    const uint8_t b[] = { 0xFF, 0x02, 0x00, 0xFD, 0x07, 0x00, 0xFE, 0x09, 0x00, 0xFC };
    tt_rx_feed(&r, a, sizeof a, tt_cb, NULL);
    CHECK_EQ_INT(tt_count, 1);
    tt_rx_feed(&r, b, sizeof b, tt_cb, NULL);
    CHECK_EQ_INT(tt_count, 4);
    CHECK_EQ_INT(tt_ev[0], TT_FRAME);
    CHECK_EQ_INT(tt_n[0], 5);
    CHECK_EQ_MEM(tt_f[0], a, 5);
    CHECK_EQ_INT(tt_ev[1], TT_FRAME);
    CHECK_EQ_INT(tt_n[1], 3);
    const uint8_t z[] = { 0x01, 0x00, 0x02 };
    CHECK_EQ_MEM(tt_f[1], z, 3);
    CHECK_EQ_INT(tt_ev[2], TT_FRAMING);
    CHECK_EQ_INT(tt_ev[3], TT_BAD_CRC);
    CHECK_EQ_INT(tt_n[3], 1);                             // only what followed the error

    // Too long for LLAP: dropped as an overflow, and the next frame is clean.
    tt_count = 0;
    static uint8_t big[LLAP_MAX + 10];
    memset(big, 0x55, sizeof big);
    const uint8_t fin[] = { 0x00, 0xFD };
    tt_rx_feed(&r, big, sizeof big, tt_cb, NULL);
    tt_rx_feed(&r, fin, sizeof fin, tt_cb, NULL);
    tt_rx_feed(&r, a, 7, tt_cb, NULL);
    CHECK_EQ_INT(tt_count, 2);
    CHECK_EQ_INT(tt_ev[0], TT_OVERFLOW);
    CHECK_EQ_INT(tt_ev[1], TT_FRAME);
    CHECK_EQ_INT(tt_n[1], 5);

    // Transmit: 0x01, the frame, its FCS low byte first.
    uint8_t cmd[1 + LLAP_MAX + 2];
    const uint8_t want[] = { 0x01, 0xFF, 0x53, 0x84, 0x8C, 0x3B };
    CHECK_EQ_INT(tt_send_cmd(cmd, sizeof cmd, a, 3), sizeof want);
    CHECK_EQ_MEM(cmd, want, sizeof want);
    CHECK_EQ_INT(tt_send_cmd(cmd, 5, a, 3), 0);

    // Node bitmap: byte n/8, bit n%8 (tashrouter's divmod).
    uint8_t nodes[33];
    CHECK_EQ_INT(tt_nodes_cmd(nodes, 200), 33);
    CHECK_EQ_INT(nodes[0], TT_CMD_NODES);
    CHECK_EQ_INT(nodes[1 + 25], 0x01);
    int set = 0;
    for (int i = 1; i < 33; i++) set += nodes[i] != 0;
    CHECK_EQ_INT(set, 1);
    tt_nodes_cmd(nodes, 129);
    CHECK_EQ_INT(nodes[1 + 16], 0x02);
    tt_nodes_cmd(nodes, 0);
    set = 0;
    for (int i = 1; i < 33; i++) set += nodes[i] != 0;
    CHECK_EQ_INT(set, 0);
}

static void test_ddp(void)
{
    // The bench LkUp's first 16 bytes (short DDP, socket 251 -> 2, NBP LkUp
    // id 0x1E from 83:251), finished with =:ImageWriter@* and its length to match.
    const uint8_t lk[] = { 0xFF, 0x53, 0x01, 0x00, 0x1C, 0x02, 0xFB, 0x02, 0x21, 0x1E,
                           0x00, 0x00, 0x53, 0xFB, 0x00, 0x01, '=',  0x0B,
                           'I', 'm', 'a', 'g', 'e', 'W', 'r', 'i', 't', 'e', 'r', 0x01, '*' };
    ddp_t d;
    uint8_t f[LLAP_MAX];
    CHECK_EQ_INT(ddp_parse(lk, sizeof lk - 1, &d), -1);   // one byte short of its length field
    memcpy(f, lk, sizeof lk);
    CHECK_EQ_INT(ddp_parse(f, sizeof lk, &d), 0);
    CHECK_EQ_INT(d.is_long, 0);
    CHECK_EQ_INT(d.src.node, 83);
    CHECK_EQ_INT(d.src.sock, 0xFB);
    CHECK_EQ_INT(d.dst.node, 0xFF);
    CHECK_EQ_INT(d.dst.sock, SOCK_NIS);
    CHECK_EQ_INT(d.type, DDP_NBP);
    CHECK_EQ_INT(d.len, sizeof lk - 3 - DDP_SHORT_HDR);

    nbp_t q;
    CHECK_EQ_INT(nbp_parse(d.data, d.len, &q), 0);
    CHECK_EQ_INT(q.fn, NBP_LKUP);
    CHECK_EQ_INT(q.count, 1);
    CHECK_EQ_INT(q.id, 0x1E);
    CHECK_EQ_INT(q.t.addr.node, 83);
    CHECK_EQ_INT(q.t.addr.sock, 0xFB);
    CHECK_EQ_INT(q.t.type[0], 11);
    CHECK_EQ_MEM(q.t.type + 1, "ImageWriter", 11);
    CHECK_EQ_INT(nbp_parse(d.data, d.len - 1, &q), -1);  // zone cut short

    // A long header round trip, with a checksum on the way in.
    lt_addr_t a = { 0x1234, 0x56, 0x78 }, b = { 0x0BCD, 0xEF, 0x02 };
    const uint8_t pay[] = { 1, 2, 3, 4, 5 };
    size_t n = ddp_build(f, sizeof f, 0x22, 0x33, 1, &a, &b, DDP_AEP, pay, sizeof pay);
    CHECK_EQ_INT(n, 3 + 13 + 5);
    CHECK_EQ_INT(f[2], LLAP_LONG);
    uint16_t ck = ddp_cksum(f + 3 + 4, n - 3 - 4);
    f[3 + 2] = (uint8_t)(ck >> 8);
    f[3 + 3] = (uint8_t)ck;
    CHECK_EQ_INT(ddp_parse(f, n, &d), 0);
    CHECK_EQ_INT(d.dst.net, 0x1234);
    CHECK_EQ_INT(d.src.net, 0x0BCD);
    CHECK_EQ_INT(d.dst.node, 0x56);
    CHECK_EQ_INT(d.src.sock, 0x02);
    CHECK_EQ_MEM(d.data, pay, sizeof pay);
    f[n - 1] ^= 0xFF;                                     // corrupt: the checksum catches it
    CHECK_EQ_INT(ddp_parse(f, n, &d), -1);
    CHECK_EQ_INT(ddp_build(f, 20, 1, 2, 1, &a, &b, DDP_AEP, pay, sizeof pay), 0);
}

static void test_nbp_match(void)
{
    uint8_t name[NBP_NAME_MAX + 1], pat[NBP_NAME_MAX + 1];
    nbp_name(name, "ImageWriter II");
    CHECK_EQ_INT(name[0], 14);
    nbp_name(pat, "=");                 CHECK(nbp_match(pat, name));
    nbp_name(pat, "imagewriter ii");    CHECK(nbp_match(pat, name));
    nbp_name(pat, "ImageWriter");       CHECK(!nbp_match(pat, name));
    nbp_name(pat, "ImageWriter III");   CHECK(!nbp_match(pat, name));
    nbp_name(pat, "==");                CHECK(!nbp_match(pat, name));
    pat[0] = 6; memcpy(pat + 1, "Image\xC5", 6);          CHECK(nbp_match(pat, name));
    pat[0] = 4; memcpy(pat + 1, "\xC5 II", 4);            CHECK(nbp_match(pat, name));
    pat[0] = 5; memcpy(pat + 1, "Im\xC5II", 5);           CHECK(nbp_match(pat, name));
    pat[0] = 5; memcpy(pat + 1, "Im\xC5XX", 5);           CHECK(!nbp_match(pat, name));
    pat[0] = 1; pat[1] = 0xC5;                            CHECK(nbp_match(pat, name));

    // UTF-8 in the queue name: one '?' per character.
    nbp_name(name, "Caf\xC3\xA9 Writer");
    CHECK_EQ_INT(name[0], 11);
    CHECK_EQ_MEM(name + 1, "Caf? Writer", 11);
    nbp_name(name, "0123456789012345678901234567890123456789");
    CHECK_EQ_INT(name[0], NBP_NAME_MAX);
}

static void test_acquire(void)
{
    lt_node_t n;
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 0, 12345 };
    claimed = -1;
    reset_fake();
    lt_node_init(&n, &cfg, &OPS, 0);
    CHECK_EQ_INT(claimed, 0);                             // answer for nothing yet
    CHECK_EQ_INT(n.phase, LT_ACQUIRING);
    uint8_t cand = n.node;
    CHECK(cand >= 128 && cand <= 254);

    lt_node_tick(&n, 0);
    CHECK_EQ_INT(nsent, 1);
    const uint8_t enq[] = { cand, cand, LLAP_ENQ };
    CHECK_EQ_INT(sent_len[0], 3);
    CHECK_EQ_MEM(sent[0], enq, 3);
    lt_node_tick(&n, 5);                                  // not due yet
    CHECK_EQ_INT(nsent, 1);

    // Someone answers: the ID is taken; the next tick moves on.
    const uint8_t ack[] = { cand, cand, LLAP_ACK };
    lt_node_rx(&n, ack, sizeof ack, 6);
    lt_node_tick(&n, 10);
    CHECK(n.node != cand);
    CHECK(n.node >= 128 && n.node <= 254);
    CHECK_EQ_INT(n.candidates, 2);
    CHECK_EQ_INT(n.enq_sent, 1);                          // restarted and sent the first
    CHECK_EQ_INT(sent[1][0], n.node);

    // A broadcast does not count against the candidate.
    const uint8_t bc[] = { 0xFF, 83, LLAP_RTS };
    lt_node_rx(&n, bc, sizeof bc, 11);
    CHECK_EQ_INT(n.clash, 0);
    // Another node's data from the candidate does.
    const uint8_t from[] = { 0xFF, n.node, LLAP_SHORT };
    lt_node_rx(&n, from, sizeof from, 12);
    CHECK_EQ_INT(n.clash, 1);
    lt_node_tick(&n, 20);
    uint8_t mine = n.node;

    uint64_t t = 20;
    for (int i = 0; i < LT_ENQ_TRIES; i++) { t += LT_ENQ_GAP_MS; lt_node_tick(&n, t); }
    CHECK_EQ_INT(n.phase, LT_ONLINE);
    CHECK_EQ_INT(claimed, mine);
    CHECK_EQ_INT(n.enq_sent, LT_ENQ_TRIES);
    CHECK(strstr(lastlog, "is ours") != NULL);

    // A hint from last boot is tried first.
    cfg.hint = 200;
    lt_node_init(&n, &cfg, &OPS, 0);
    CHECK_EQ_INT(n.node, 200);
    cfg.hint = 7;                                         // a user-range ID is not a server's
    lt_node_init(&n, &cfg, &OPS, 0);
    CHECK(n.node >= 128);
}

static void test_lookup(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    CHECK_EQ_INT(claimed, 200);

    uint8_t nbp[64], f[LLAP_MAX];
    size_t k = 0;
    nbp[k++] = 0x21; nbp[k++] = 0x1E;                     // LkUp, 1 tuple, id 30
    nbp[k++] = 0; nbp[k++] = 0; nbp[k++] = 83; nbp[k++] = 0xFB; nbp[k++] = 0;
    nbp[k++] = 1; nbp[k++] = '=';
    nbp[k++] = 11; memcpy(nbp + k, "ImageWriter", 11); k += 11;
    nbp[k++] = 1; nbp[k++] = '*';
    size_t fl = mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(n.st.lookups, 1);
    CHECK_EQ_INT(n.st.replies, 1);
    const uint8_t want[] = {
        83, 200, LLAP_SHORT,
        0x00, 5 + 2 + 5 + 15 + 12 + 2, 0xFB, SOCK_NIS, DDP_NBP,
        0x31, 0x1E, 0x00, 0x00, 200, LT_SOCK_SLS, 0x00,
        14, 'I','m','a','g','e','W','r','i','t','e','r',' ','I','I',
        11, 'I','m','a','g','e','W','r','i','t','e','r',
        1, '*',
    };
    CHECK_EQ_INT(sent_len[0], sizeof want);
    CHECK_EQ_MEM(sent[0], want, sizeof want);
    CHECK(strstr(lastlog, "=:ImageWriter@* from 83:251 -> answered") != NULL);

    // The same lookup a second later answers again but logs nothing new.
    int logs = nlogs;
    lt_node_rx(&n, f, fl, t + 1000);
    CHECK_EQ_INT(nsent, 2);
    CHECK_EQ_INT(nlogs, logs);

    // Another type, or another object, is not us.
    k = 9;
    nbp[k++] = 11; memcpy(nbp + k, "LaserWriter", 11); k += 11;
    nbp[k++] = 1; nbp[k++] = '*';
    fl = mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t + 2000);
    CHECK_EQ_INT(nsent, 2);
    CHECK(strstr(lastlog, "not us") != NULL);

    // A lookup addressed to another node is ignored outright.
    k = 9;
    nbp[k++] = 11; memcpy(nbp + k, "ImageWriter", 11); k += 11;
    nbp[k++] = 1; nbp[k++] = '*';
    fl = mac_ddp(f, 99, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t + 3000);
    CHECK_EQ_INT(nsent, 2);

    // RTMP from a router gives the network number, used in the next reply.
    const uint8_t rtmp[] = { 0x00, 0x07, 0x08, 0x05 };
    lt_addr_t rto = { 0, 0xFF, SOCK_RTMP }, rfrom = { 0, 5, SOCK_RTMP };
    fl = ddp_build(f, sizeof f, 0xFF, 5, 0, &rto, &rfrom, DDP_RTMP, rtmp, sizeof rtmp);
    lt_node_rx(&n, f, fl, t + 4000);
    CHECK_EQ_INT(n.net, 7);
    fl = mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t + 5000);
    CHECK_EQ_INT(nsent, 3);
    CHECK_EQ_INT(sent[2][3 + 5 + 2], 0x00);
    CHECK_EQ_INT(sent[2][3 + 5 + 3], 0x07);               // tuple net 7
}

static void test_aep(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 150);
    uint8_t f[LLAP_MAX];
    const uint8_t echo[] = { 1, 'h', 'i' };
    size_t fl = mac_ddp(f, 150, SOCK_AEP, DDP_AEP, echo, sizeof echo);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);
    const uint8_t want[] = { 83, 150, LLAP_SHORT, 0, 8, 0xFB, SOCK_AEP, DDP_AEP, 2, 'h', 'i' };
    CHECK_EQ_MEM(sent[0], want, sizeof want);
    CHECK(n.heard[83 / 8] & (1u << (83 % 8)));

    // Our own echo request, and the Mac's reply.
    reset_fake();
    lt_node_echo(&n, 83, t + 100);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(sent[0][0], 83);
    CHECK_EQ_INT(sent[0][3 + 2], SOCK_AEP);
    CHECK_EQ_INT(sent[0][3 + 3], SOCK_AEP);
    CHECK_EQ_INT(sent[0][3 + 5], 1);
    uint8_t rep[] = { 2, 'x' };
    lt_addr_t to = { 0, 150, SOCK_AEP }, from = { 0, 83, SOCK_AEP };
    fl = ddp_build(f, sizeof f, 150, 83, 0, &to, &from, DDP_AEP, rep, sizeof rep);
    lt_node_rx(&n, f, fl, t + 112);
    CHECK_EQ_INT(n.st.echoes, 1);
    CHECK_EQ_INT(nsent, 1);                               // a reply is not echoed back
    CHECK(strstr(lastlog, "echo reply from 83, 2 bytes, 12 ms") != NULL);
}

// The ATP header of sent frame i (short DDP): ctl, bitmap, tid, user[4].
static const uint8_t *atp_of(int i) { return sent[i] + 3 + DDP_SHORT_HDR; }

static void test_pap(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    uint8_t f[LLAP_MAX];

    // SendStatus at the SLS. The Option Card's answer: four unused bytes, then
    // the word as a two-byte Pascal string, low byte first. Deselected: 0x0008.
    lt_node_set_status(&n, LT_IW_OFFLINE);
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ, 0x01, 0x0101, 0, PAP_SEND_STATUS, 0, 0, NULL, 0);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);
    const uint8_t st[] = { ATP_TRESP | ATP_EOM, 0, 0x01, 0x01, 0, PAP_STATUS, 0, 0,
                           0, 0, 0, 0, 2, 0x08, 0x00 };
    CHECK_EQ_INT(sent_len[0], 3 + 5 + sizeof st);
    CHECK_EQ_MEM(atp_of(0), st, sizeof st);
    CHECK_EQ_INT(sent[0][3 + 2], 0xFB);                   // back to the asking socket
    CHECK_EQ_INT(sent[0][3 + 3], LT_SOCK_SLS);

    // OpenConn: ConnID 0x42, workstation socket 0xFC, quantum 8, wait 0.
    reset_fake();
    lt_node_set_status(&n, 0x0000);
    const uint8_t oc[] = { 0xFC, 8, 0, 0 };
    fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0202, 0x42, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 2);                               // OpenConnReply, then our SendData
    const uint8_t ocr[] = { ATP_TRESP | ATP_EOM, 0, 0x02, 0x02, 0x42, PAP_OPEN_CONN_REPLY, 0, 0,
                            LT_SOCK_CONN, LT_FLOW, 0x00, 0x00, 2, 0x00, 0x00 };
    CHECK_EQ_MEM(atp_of(0), ocr, sizeof ocr);
    CHECK(n.pap.open);
    const uint8_t *sd = atp_of(1);
    CHECK_EQ_INT(sent[1][3 + 2], 0xFC);                   // to the workstation's socket
    CHECK_EQ_INT(sent[1][3 + 3], LT_SOCK_CONN);
    CHECK_EQ_INT(sd[0], ATP_TREQ | ATP_XO);
    CHECK_EQ_INT(sd[1], 0xFF);                            // 8 packets
    CHECK_EQ_INT(sd[4], 0x42);
    CHECK_EQ_INT(sd[5], PAP_SEND_DATA);
    CHECK_EQ_INT(sd[6], 0);
    CHECK_EQ_INT(sd[7], 1);                               // sequence 1
    uint16_t tid1 = (uint16_t)(sd[2] << 8 | sd[3]);

    // The Mac missed our reply and asks again: same answer, no second read.
    reset_fake();
    lt_node_rx(&n, f, fl, t + 2000);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_MEM(atp_of(0), ocr, sizeof ocr);

    // Another Mac while we are open: busy.
    reset_fake();
    uint8_t g[LLAP_MAX];
    size_t gl = mac_atp(g, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0303, 0x43, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    g[1] = 84;                                            // from node 84
    lt_node_rx(&n, g, gl, t + 2100);
    CHECK_EQ_INT(nsent, 1);
    const uint8_t busy[] = { ATP_TRESP | ATP_EOM, 0, 0x03, 0x03, 0x43, PAP_OPEN_CONN_REPLY, 0, 0,
                             0, LT_FLOW, 0xFF, 0xFF, 2, 0x00, 0x80 };   // the busy bit is ours
    CHECK_EQ_MEM(atp_of(0), busy, sizeof busy);
    CHECK_EQ_INT(sent[0][0], 84);
    CHECK_EQ_INT(n.st.busy, 1);

    // SendStatus while open shows busy too; the set word is kept underneath.
    reset_fake();
    gl = mac_atp(g, 200, LT_SOCK_SLS, ATP_TREQ, 0x01, 0x0304, 0, PAP_SEND_STATUS, 0, 0, NULL, 0);
    g[1] = 84;
    lt_node_set_status(&n, LT_IW_OFFLINE | LT_IW_BUSY);
    lt_node_rx(&n, g, gl, t + 2200);
    CHECK_EQ_INT(atp_of(0)[13], 0x08);
    CHECK_EQ_INT(atp_of(0)[14], 0x80);
    CHECK_EQ_INT(n.status, LT_IW_OFFLINE);

    // The job: two Data packets, the second with EOM and EOF.
    reset_fake();
    uint8_t d0[512], d1[100];
    for (int i = 0; i < 512; i++) d0[i] = (uint8_t)i;
    for (int i = 0; i < 100; i++) d1[i] = (uint8_t)(0xA0 + i);
    // Out of order: sequence 1 first.
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 1, tid1, 0x42, PAP_DATA, 1, 0, d1, sizeof d1);
    lt_node_rx(&n, f, fl, t + 3000);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(n.pap.rd_want, 0x01);                    // EOM at 1: only 0 still wanted
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP, 0, tid1, 0x42, PAP_DATA, 1, 0, d0, sizeof d0);
    lt_node_rx(&n, f, fl, t + 3001);
    CHECK_EQ_INT(nsent, 1);                               // TRel, and no next read after EOF
    CHECK_EQ_INT(atp_of(0)[0], ATP_TREL);
    CHECK_EQ_INT((atp_of(0)[2] << 8 | atp_of(0)[3]), tid1);
    CHECK_EQ_INT(job_len, 612);
    CHECK_EQ_MEM(job, d0, 512);
    CHECK_EQ_MEM(job + 512, d1, 100);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(n.pap.bytes, 612);

    // The Mac reads us back: an empty EOF.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0xFF, 0x0404, 0x42, PAP_SEND_DATA, 0, 1, NULL, 0);
    lt_node_rx(&n, f, fl, t + 3100);
    CHECK_EQ_INT(nsent, 1);
    const uint8_t eof[] = { ATP_TRESP | ATP_EOM, 0, 0x04, 0x04, 0x42, PAP_DATA, 1, 0 };
    CHECK_EQ_INT(sent_len[0], 3 + 5 + sizeof eof);
    CHECK_EQ_MEM(atp_of(0), eof, sizeof eof);

    // CloseConn.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0505, 0x42, PAP_CLOSE_CONN, 0, 0, NULL, 0);
    lt_node_rx(&n, f, fl, t + 3200);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(atp_of(0)[5], PAP_CLOSE_CONN_REPLY);
    CHECK(!n.pap.open);
    CHECK_EQ_INT(job_eof, 1);                             // not delivered twice
    CHECK(strstr(lastlog, "closed (by the Mac) after 612 bytes") != NULL);
}

static void test_pap_timers(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xFC, 8, 0, 0 };
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0202, 0x42, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    uint16_t tid1 = (uint16_t)(atp_of(1)[2] << 8 | atp_of(1)[3]);

    // The read goes unanswered: retried after 15 s with the same TID.
    reset_fake();
    lt_node_tick(&n, t + LT_PAP_SENDDATA_RETRY_MS - 1);
    CHECK_EQ_INT(nsent, 0);
    lt_node_tick(&n, t + LT_PAP_SENDDATA_RETRY_MS);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(atp_of(0)[5], PAP_SEND_DATA);
    CHECK_EQ_INT((atp_of(0)[2] << 8 | atp_of(0)[3]), tid1);

    // A tickle from the Mac keeps it alive; ours goes out every 60 s.
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ, 0x01, 0x0606, 0x42, PAP_TICKLE, 0, 0, NULL, 0);
    reset_fake();
    lt_node_rx(&n, f, fl, t + 59000);
    CHECK_EQ_INT(nsent, 0);                               // tickles get no response
    lt_node_tick(&n, t + LT_PAP_TICKLE_MS);
    int tickles = 0;
    for (int i = 0; i < nsent && i < MAXSENT; i++) tickles += atp_of(i)[5] == PAP_TICKLE;
    CHECK_EQ_INT(tickles, 1);

    // Silence for two minutes after the last tickle: torn down.
    lt_node_tick(&n, t + 59000 + LT_PAP_CONN_TIMEOUT_MS - 1);
    CHECK(n.pap.open);
    job_eof = 0;
    lt_node_tick(&n, t + 59000 + LT_PAP_CONN_TIMEOUT_MS);
    CHECK(!n.pap.open);
    CHECK_EQ_INT(job_eof, 1);
    CHECK(strstr(lastlog, "timed out") != NULL);
}

int main(void)
{
    test_fcs();
    test_tashtalk();
    test_ddp();
    test_nbp_match();
    test_acquire();
    test_lookup();
    test_aep();
    test_pap();
    test_pap_timers();
    TEST_END;
}
