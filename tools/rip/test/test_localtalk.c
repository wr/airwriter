// SPDX-License-Identifier: GPL-3.0-or-later
// LocalTalk node (W-1051): packet layouts and the node state machine, driven
// with a fake clock and a fake TashTalk that records what the node sends.
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "lt_tashtalk.h"
#include "serial_writer.h"

// ------------------------------------------------------------- the fake

#define MAXSENT 64
static uint8_t  sent[MAXSENT][LLAP_MAX];
static size_t   sent_len[MAXSENT];
static int      nsent;
static int      claimed = -1;
static char     lastlog[160];
static int      nlogs;
static uint8_t  job[16384];
static size_t   job_len;
static int      job_eof;                  // job_end calls
static int      job_normal = -1;          // the last job_end's verdict
static int      job_begins, fake_busy;
static size_t   fake_room = (size_t)-1;   // the printer's free space

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
static int f_begin(void *ctx) { (void)ctx; job_begins++; return fake_busy; }
static void f_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (fake_room != (size_t)-1) {
        CHECK(n <= fake_room);                            // never more than room said
        fake_room -= n < fake_room ? n : fake_room;
    }
    if (n && job_len + n <= sizeof job) { memcpy(job + job_len, d, n); job_len += n; }
}
static size_t f_room(void *ctx) { (void)ctx; return fake_room; }
static void f_end(void *ctx, int normal) { (void)ctx; job_eof++; job_normal = normal; }

static const lt_ops_t OPS = { NULL, f_send, f_claim, f_log, f_begin, f_data, f_room, f_end, NULL,
                              NULL, NULL, NULL, NULL };

static void reset_fake(void) { nsent = 0; nlogs = 0; lastlog[0] = 0; }

// Bring a node online at `id` with no clash: 120 enquiries 10 ms apart.
static uint64_t online(lt_node_t *n, uint8_t id)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, id, 1, NULL, NULL };
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

    // UTF-8 in a printer's name: MacRoman where it has the character (W-1088),
    // one '?' per character where it has not.
    nbp_name(name, "Caf\xC3\xA9 Writer");
    CHECK_EQ_INT(name[0], 11);
    CHECK_EQ_MEM(name + 1, "Caf\x8E Writer", 11);
    nbp_name(name, "Wells\xE2\x80\x99s \xE2\x86\x92 IW");      // the curly apostrophe; an arrow
    CHECK_EQ_INT(name[0], 12);
    CHECK_EQ_MEM(name + 1, "Wells\xD5s ? IW", 12);
    nbp_name(name, "0123456789012345678901234567890123456789");
    CHECK_EQ_INT(name[0], NBP_NAME_MAX);
}

static void test_acquire(void)
{
    lt_node_t n;
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 0, 12345, NULL, NULL };
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
                             0, LT_FLOW, 0xFF, 0xFF, 2, 0x00, 0x00 };
    CHECK_EQ_MEM(atp_of(0), busy, sizeof busy);
    CHECK_EQ_INT(sent[0][0], 84);
    CHECK_EQ_INT(n.st.busy, 1);

    // SendStatus while a Mac's connection is open does not say busy: a Mac
    // told so about its own connection never sends (bench). Busy is only set
    // from outside, while the board prints someone else's job.
    reset_fake();
    gl = mac_atp(g, 200, LT_SOCK_SLS, ATP_TREQ, 0x01, 0x0304, 0, PAP_SEND_STATUS, 0, 0, NULL, 0);
    lt_node_rx(&n, g, gl, t + 2200);
    CHECK_EQ_INT(atp_of(0)[13], 0x00);
    CHECK_EQ_INT(atp_of(0)[14], 0x00);
    reset_fake();
    lt_node_set_status(&n, LT_IW_OFFLINE | LT_IW_BUSY);
    lt_node_rx(&n, g, gl, t + 2300);
    CHECK_EQ_INT(atp_of(0)[13], 0x08);
    CHECK_EQ_INT(atp_of(0)[14], 0x80);
    lt_node_set_status(&n, 0x0000);

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
    CHECK_EQ_INT(job_normal, 1);                          // EOF ends it normally
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
    CHECK_EQ_INT(job_normal, 0);                          // abandoned
    CHECK(strstr(lastlog, "timed out") != NULL);
}

// The Mac reads us while it writes. Its read must not be held to the end:
// held, the driver stops at the end of the page and never sends EOF (bench).
static void test_pap_mac_reads(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0202, 0xAD, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    uint16_t tid1 = (uint16_t)(atp_of(1)[2] << 8 | atp_of(1)[3]);

    // A chunk arrives, and the Mac's read (seq 1) comes in behind it.
    const uint8_t chunk[] = { 0x1B, 'G', '0', '0', '0', '1', 0x55 };
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid1, 0xAD, PAP_DATA, 0, 0, chunk, sizeof chunk);
    lt_node_rx(&n, f, fl, t + 100);
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0700, 0xAD, PAP_SEND_DATA, 0, 1, NULL, 0);
    lt_node_rx(&n, f, fl, t + 150);
    CHECK_EQ_INT(nsent, 0);                               // still writing: held
    CHECK(strstr(lastlog, "the Mac reads us too (seq 1)") != NULL);
    CHECK_EQ_INT(n.st.ws_reads, 1);
    lt_node_tick(&n, t + 100 + LT_PAP_READ_IDLE_MS - 1);
    CHECK_EQ_INT(nsent, 0);

    // Quiet for LT_PAP_READ_IDLE_MS: answered empty, no EOF.
    lt_node_tick(&n, t + 100 + LT_PAP_READ_IDLE_MS);
    CHECK_EQ_INT(nsent, 1);
    const uint8_t keep[] = { ATP_TRESP | ATP_EOM, 0, 0x07, 0x00, 0xAD, PAP_DATA, 0, 0 };
    CHECK_EQ_INT(sent_len[0], 3 + 5 + sizeof keep);
    CHECK_EQ_MEM(atp_of(0), keep, sizeof keep);
    CHECK_EQ_INT(sent[0][3 + 2], 0xFB);                   // to the socket that asked
    CHECK_EQ_INT(n.st.ws_answers, 1);

    // A PAP-level retransmit (fresh TID, same sequence) gets the same answer
    // at once; an ATP-level one (same TID) comes from the XO cache.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0701, 0xAD, PAP_SEND_DATA, 0, 1, NULL, 0);
    lt_node_rx(&n, f, fl, t + 2200);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT((atp_of(0)[2] << 8 | atp_of(0)[3]), 0x0701);
    CHECK_EQ_INT(atp_of(0)[6], 0);

    // The next read (seq 2) waits for the next quiet spell.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0702, 0xAD, PAP_SEND_DATA, 0, 2, NULL, 0);
    lt_node_rx(&n, f, fl, t + 2300);
    CHECK_EQ_INT(nsent, 1);                               // quiet since t+100: answered now
    CHECK_EQ_INT((atp_of(0)[2] << 8 | atp_of(0)[3]), 0x0702);

    // The job's EOF arrives on our read; the Mac's next read gets EOF, once.
    reset_fake();
    const uint8_t last[] = { 0x0C };
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, n.pap.rd_tid, 0xAD, PAP_DATA, 1, 0, last, sizeof last);
    lt_node_rx(&n, f, fl, t + 3000);
    CHECK(n.pap.done);
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0703, 0xAD, PAP_SEND_DATA, 0, 3, NULL, 0);
    lt_node_rx(&n, f, fl, t + 3010);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(atp_of(0)[6], 1);                        // EOF
    CHECK(strstr(lastlog, "EOF to the Mac's read") != NULL);
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0704, 0xAD, PAP_SEND_DATA, 0, 4, NULL, 0);
    lt_node_rx(&n, f, fl, t + 3020);
    CHECK_EQ_INT(nsent, 0);                               // after EOF: keep-alives only
    lt_node_tick(&n, t + 3000 + LT_PAP_READ_IDLE_MS);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(atp_of(0)[6], 0);
}

// The Mac gives up a connection without CloseConn and opens another.
static void test_pap_superseded(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0100, 0xAD, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    CHECK(n.pap.open);

    // Soon after: still busy, even from the same Mac.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0101, 0x01, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t + LT_PAP_STALE_MS - 1);
    CHECK_EQ_INT(atp_of(0)[10], 0xFF);
    CHECK_EQ_INT(n.pap.connid, 0xAD);

    // Another Mac, however long the quiet: busy.
    reset_fake();
    uint8_t g[LLAP_MAX];
    size_t gl = mac_atp(g, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0102, 0x02, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    g[1] = 84;
    lt_node_rx(&n, g, gl, t + LT_PAP_STALE_MS + 5000);
    CHECK_EQ_INT(atp_of(0)[10], 0xFF);
    CHECK_EQ_INT(n.pap.connid, 0xAD);

    // The same Mac after LT_PAP_STALE_MS with no data: the old one closes and
    // the new one opens.
    reset_fake();
    job_eof = 0;
    fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0103, 0x01, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t + LT_PAP_STALE_MS + 6000);
    CHECK_EQ_INT(atp_of(0)[10], 0x00);
    CHECK_EQ_INT(atp_of(0)[11], 0x00);
    CHECK(n.pap.open);
    CHECK_EQ_INT(n.pap.connid, 0x01);
    CHECK_EQ_INT(job_eof, 1);                             // the old job was ended
    CHECK_EQ_INT(job_normal, 0);
    CHECK_EQ_INT(n.st.opens, 2);
}

// The printer's side: busy, back-pressure, and giving up.
static void test_pap_printer(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };

    // An IPP job holds the printer: busy, and no connection.
    fake_busy = 1;
    job_begins = 0;
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0300, 0x10, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(job_begins, 1);
    CHECK(!n.pap.open);
    CHECK_EQ_INT(atp_of(0)[10], 0xFF);
    CHECK_EQ_INT(atp_of(0)[11], 0xFF);
    CHECK(strstr(lastlog, "while the printer is busy") != NULL);

    // Free again, but with less room than a read can bring: open, no read.
    fake_busy = 0;
    fake_room = LT_READ_MAX - 1;
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0301, 0x11, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t + 2000);
    CHECK(n.pap.open);
    CHECK_EQ_INT(job_begins, 2);
    CHECK_EQ_INT(nsent, 1);                               // OpenConnReply only
    lt_node_tick(&n, t + 2100);
    CHECK_EQ_INT(nsent, 1);
    fake_room = LT_READ_MAX;                              // the printer caught up
    lt_node_tick(&n, t + 2200);
    CHECK_EQ_INT(nsent, 2);
    CHECK_EQ_INT(atp_of(1)[5], PAP_SEND_DATA);
    CHECK_EQ_INT(atp_of(1)[7], 1);

    // The printer gave up: CloseConn to the Mac, the job ends abandoned.
    reset_fake();
    job_eof = 0;
    lt_node_pap_abort(&n, t + 3000);
    CHECK(!n.pap.open);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(job_normal, 0);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(sent[0][3 + 2], 0xF4);                   // the Mac's responding socket
    CHECK_EQ_INT(atp_of(0)[0], ATP_TREQ);
    CHECK_EQ_INT(atp_of(0)[4], 0x11);
    CHECK_EQ_INT(atp_of(0)[5], PAP_CLOSE_CONN);
    lt_node_pap_abort(&n, t + 3001);                      // nothing open: nothing sent
    CHECK_EQ_INT(nsent, 1);
    fake_room = (size_t)-1;
}

// A Mac that stops sending mid-job, tickling all the while, is closed after
// LT_PAP_QUIET_MS -- unless the printer is not ready, which it is waiting out.
static void test_pap_quiet(void)
{
    for (int offline = 0; offline <= 1; offline++) {
        lt_node_t n;
        uint64_t t = online(&n, 200);
        uint8_t f[LLAP_MAX];
        const uint8_t oc[] = { 0xF4, 1, 0, 0 };
        size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0600, 0x21, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
        lt_node_rx(&n, f, fl, t);
        uint16_t tid1 = (uint16_t)(atp_of(1)[2] << 8 | atp_of(1)[3]);
        const uint8_t chunk[] = { 0x0D, 0x0A };
        fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid1, 0x21, PAP_DATA, 0, 0, chunk, sizeof chunk);
        lt_node_rx(&n, f, fl, t + 100);
        CHECK(n.pap.rd_want);                             // the next read is out
        if (offline) lt_node_set_status(&n, LT_IW_OFFLINE);
        job_eof = 0;
        job_normal = -1;
        // Tickles keep the connection alive; ticks run the clock.
        uint64_t end = t + 100 + LT_PAP_QUIET_MS * (offline ? 2 : 1);
        for (uint64_t now = t + 1000; now < end; now += 1000) {
            if ((now - t) % 30000 == 0) {
                fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ, 0x01, (uint16_t)now, 0x21, PAP_TICKLE, 0, 0, NULL, 0);
                lt_node_rx(&n, f, fl, now);
            }
            lt_node_tick(&n, now);
        }
        CHECK(n.pap.open);
        reset_fake();
        lt_node_tick(&n, end + 1000);
        if (offline) {
            CHECK(n.pap.open);                            // paper-out is not quiet
            CHECK_EQ_INT(job_eof, 0);
            lt_node_set_status(&n, 0);
        } else {
            CHECK(!n.pap.open);
            CHECK_EQ_INT(job_eof, 1);
            CHECK_EQ_INT(job_normal, 1);                  // what came prints
            int closes = 0;
            for (int i = 0; i < nsent && i < MAXSENT; i++)
                closes += atp_of(i)[5] == PAP_CLOSE_CONN && atp_of(i)[0] == ATP_TREQ;
            CHECK_EQ_INT(closes, 1);
            CHECK(strstr(lastlog, "went quiet") != NULL);
        }
    }
}

// The captured page, fed as the Mac sends it (one ~88-byte Data per read,
// flow quantum 1, CloseConn at the end), through a spool and serial_writer
// into a printer FIFO that takes 16 bytes at a time: what reaches the wire
// must be the page, byte for byte, with reads held while the spool is full.
static uint8_t  spool[6000];
static size_t   sp_head, sp_used;
static uint8_t  wire[16384];
static size_t   wire_len;

static int sp_begin(void *ctx) { (void)ctx; return 0; }
static void sp_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    CHECK(n <= sizeof spool - sp_used);
    for (size_t i = 0; i < n && sp_used < sizeof spool; i++, sp_used++)
        spool[(sp_head + sp_used) % sizeof spool] = d[i];
}
static size_t sp_room(void *ctx) { (void)ctx; return sizeof spool - sp_used; }

static int fifo_write(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    size_t k = n < 16 ? n : 16;
    if (wire_len + k <= sizeof wire) memcpy(wire + wire_len, buf, k);
    wire_len += k;
    return (int)k;
}
static void fifo_delay(void *ctx) { (void)ctx; }

static void printer_drain(size_t most)
{
    serial_writer_ops_t w = { NULL, fifo_write, fifo_delay, NULL, 0 };
    uint8_t buf[256];
    while (most && sp_used) {
        size_t k = most < sp_used ? most : sp_used;
        if (k > sizeof buf) k = sizeof buf;
        for (size_t i = 0; i < k; i++) buf[i] = spool[(sp_head + i) % sizeof spool];
        sp_head = (sp_head + k) % sizeof spool;
        sp_used -= k;
        most -= k;
        CHECK_EQ_INT(serial_writer_write(&w, buf, k), 0);
    }
}

static void test_pap_fixture(void)
{
    static uint8_t page[16384];
    FILE *fp = fopen("../../fixtures/pap-iw701-page.bin", "rb");
    CHECK(fp != NULL);
    size_t len = fp ? fread(page, 1, sizeof page, fp) : 0;
    if (fp) fclose(fp);
    CHECK_EQ_INT(len, 11968);

    lt_ops_t ops = OPS;
    ops.job_begin = sp_begin;
    ops.job_data = sp_data;
    ops.job_room = sp_room;
    lt_node_t n;
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 200, 1, NULL, NULL };
    lt_node_init(&n, &cfg, &ops, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&n, t);
    sp_head = sp_used = wire_len = 0;
    job_eof = 0;
    job_normal = -1;

    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    reset_fake();
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0400, 0xAD, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);

    size_t off = 0;
    int reads = 0, held = 0, pending = 0;
    uint16_t tid = 0, last_seq = 0;
    for (int step = 0; step < 200000 && (off < len || sp_used); step++) {
        t += 10;
        // Collect the node's newest SendData, if it sent one.
        for (int i = 0; i < nsent && i < MAXSENT; i++) {
            const uint8_t *a = atp_of(i);
            if (sent[i][3 + 4] != DDP_ATP || (a[0] & ATP_FN_MASK) != ATP_TREQ || a[5] != PAP_SEND_DATA)
                continue;
            uint16_t seq = (uint16_t)(a[6] << 8 | a[7]);
            if (pending && seq == last_seq) continue;     // a retry of the one we hold
            CHECK_EQ_INT(seq, last_seq + 1);              // 1, 2, 3, ...
            tid = (uint16_t)(a[2] << 8 | a[3]);
            last_seq = seq;
            pending = 1;
        }
        reset_fake();
        if (pending && off < len) {
            size_t k = len - off < 88 ? len - off : 88;
            fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid, 0xAD, PAP_DATA, 0, 0, page + off, k);
            lt_node_rx(&n, f, fl, t);
            off += k;
            reads++;
            pending = 0;
        } else if (!pending && off < len) {
            held++;                                       // back-pressure: no read out
        }
        printer_drain(step % 3 ? 0 : 30);                 // slower than the Mac
        lt_node_tick(&n, t);
    }
    CHECK_EQ_INT(off, len);
    // The last Data is in; the Mac closes, as 7.0.1 does, without EOF.
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ, 0x01, 0x0500, 0xAD, PAP_CLOSE_CONN, 0, 0, NULL, 0);
    lt_node_rx(&n, f, fl, t + 10);
    printer_drain(sizeof spool);
    CHECK_EQ_INT(reads, 136);
    CHECK(held > 0);
    CHECK_EQ_INT(wire_len, len);
    CHECK_EQ_MEM(wire, page, len);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(job_normal, 1);
    CHECK(!n.pap.open);
}

// ------------------------------------------------------------- ELAP mode
// W-1076: the node behind et_link, which hands it each datagram as a
// pseudo-LLAP header {dst node, src node, LLAP_LONG} and a long DDP header.
// Here a Mac at 0xFF10.12, socket 0xFD, talks to the node at 0xFF23.77.

#define ENET  0xFF23
#define ENODE 77

static uint64_t elap_online(lt_node_t *n, uint32_t idle_hold_ms)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 0, 1, NULL, NULL };
    lt_node_init_elap(n, &cfg, &OPS, idle_hold_ms, 0);
    lt_node_set_address(n, ENET, ENODE, 1000);
    reset_fake();
    return 1000;
}

static size_t emac_ddp(uint8_t *f, uint16_t dnet, uint8_t dnode, uint8_t dsock, uint8_t type,
                       const uint8_t *data, size_t len)
{
    lt_addr_t to = { dnet, dnode, dsock }, from = { 0xFF10, 12, 0xFD };
    return ddp_build(f, LLAP_MAX, dnode, 12, 1, &to, &from, type, data, len);
}

static size_t emac_atp(uint8_t *f, uint8_t dsock, uint8_t ctl, uint8_t bitmap, uint16_t tid,
                       uint8_t u0, uint8_t u1, const uint8_t *data, size_t len)
{
    uint8_t a[ATP_HDR + ATP_MAX_DATA];
    uint8_t user[4] = { u0, u1, 0, 0 };
    size_t al = atp_build(a, sizeof a, ctl, bitmap, tid, user, data, len);
    return emac_ddp(f, ENET, ENODE, dsock, DDP_ATP, a, al);
}

static const uint8_t *eatp_of(int i) { return sent[i] + 3 + DDP_LONG_HDR; }

static size_t lkup_from_ff10(uint8_t *nbp)
{
    size_t k = 0;
    nbp[k++] = 0x21; nbp[k++] = 0x1E;                     // LkUp, 1 tuple, id 30
    nbp[k++] = 0xFF; nbp[k++] = 0x10; nbp[k++] = 12; nbp[k++] = 0xFD; nbp[k++] = 0;
    nbp[k++] = 1; nbp[k++] = '=';
    nbp[k++] = 11; memcpy(nbp + k, "ImageWriter", 11); k += 11;
    nbp[k++] = 1; nbp[k++] = '*';
    return k;
}

static void test_elap_init_quiet(void)
{
    lt_node_t n;
    claimed = -1;
    reset_fake();
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 0, 1, NULL, NULL };
    lt_node_init_elap(&n, &cfg, &OPS, 0, 0);
    for (uint64_t t = 0; t <= 2000; t += 10) lt_node_tick(&n, t);
    CHECK_EQ_INT(nsent, 0);                               // no ENQs: AARP is et_link's
    CHECK_EQ_INT(claimed, -1);
    CHECK(n.phase == LT_ACQUIRING);
    // Nothing is answered before the address is ours.
    uint8_t nbp[64], f[LLAP_MAX];
    size_t fl = emac_ddp(f, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, lkup_from_ff10(nbp));
    lt_node_rx(&n, f, fl, 2000);
    CHECK_EQ_INT(nsent, 0);
    lt_node_set_address(&n, ENET, ENODE, 2000);
    CHECK(n.phase == LT_ONLINE);
    CHECK_EQ_INT(n.net, ENET);
    CHECK_EQ_INT(n.node, ENODE);
    CHECK(strstr(lastlog, "is ours") != NULL);
    CHECK_EQ_INT(claimed, -1);
}

static void test_elap_lookup(void)
{
    lt_node_t n;
    uint64_t t = elap_online(&n, 0);
    uint8_t nbp[64], f[LLAP_MAX];
    size_t fl = emac_ddp(f, 0, 0xFF, SOCK_NIS, DDP_NBP, nbp, lkup_from_ff10(nbp));
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);
    ddp_t d;
    CHECK_EQ_INT(ddp_parse(sent[0], sent_len[0], &d), 0);
    CHECK(d.is_long);
    CHECK_EQ_INT(d.dst.net, 0xFF10);
    CHECK_EQ_INT(d.dst.node, 12);
    CHECK_EQ_INT(d.dst.sock, 0xFD);
    CHECK_EQ_INT(d.src.net, ENET);
    CHECK_EQ_INT(d.src.node, ENODE);
    CHECK_EQ_INT(d.src.sock, SOCK_NIS);
    CHECK_EQ_INT(d.type, DDP_NBP);
    nbp_t r;
    CHECK_EQ_INT(nbp_parse(d.data, d.len, &r), 0);
    CHECK_EQ_INT(r.fn, NBP_LKUP_REPLY);
    CHECK_EQ_INT(r.t.addr.net, ENET);
    CHECK_EQ_INT(r.t.addr.node, ENODE);
    CHECK_EQ_INT(r.t.addr.sock, LT_SOCK_SLS);

    // An echo goes out with a long header too.
    reset_fake();
    lt_node_echo(&n, 12, t);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(sent[0][2], LLAP_LONG);
}

static void test_elap_other_net(void)
{
    lt_node_t n;
    uint64_t t = elap_online(&n, 0);
    uint8_t nbp[64], f[LLAP_MAX];
    size_t k = lkup_from_ff10(nbp);
    size_t fl = emac_ddp(f, 0xFF99, ENODE, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t);
    fl = emac_ddp(f, 0xFF99, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 0);
    fl = emac_ddp(f, ENET, ENODE, SOCK_NIS, DDP_NBP, nbp, k);   // ours, by number
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);
}

static void test_elap_rtmp(void)
{
    lt_node_t n;
    uint64_t t = elap_online(&n, 0);
    const uint8_t rtmp[] = { 0x00, 0x07, 0x08, 0x05 };
    uint8_t f[LLAP_MAX];
    lt_addr_t rto = { 0, 0xFF, SOCK_RTMP }, rfrom = { 0xFF10, 5, SOCK_RTMP };
    size_t fl = ddp_build(f, sizeof f, 0xFF, 5, 1, &rto, &rfrom, DDP_RTMP, rtmp, sizeof rtmp);
    int logs = nlogs;
    lt_node_rx(&n, f, fl, t);
    lt_node_rx(&n, f, fl, t + 10000);
    CHECK_EQ_INT(n.net, ENET);
    CHECK_EQ_INT(nlogs, logs + 1);
    CHECK(strstr(lastlog, "router") != NULL);
}

static int close_conns_sent(void)
{
    int closes = 0;
    for (int i = 0; i < nsent && i < MAXSENT; i++)
        closes += eatp_of(i)[5] == PAP_CLOSE_CONN && (eatp_of(i)[0] & ATP_FN_MASK) == ATP_TREQ;
    return closes;
}

// Run the clock from `from` to `to` (inclusive) in 10 ms ticks, the Mac
// tickling every 30 s so only the idle hold can close the connection.
static void elap_run(lt_node_t *n, uint64_t t0, uint64_t from, uint64_t to)
{
    uint8_t f[LLAP_MAX];
    for (uint64_t now = from; now <= to; now += 10) {
        if ((now - t0) % 30000 == 0) {
            size_t fl = emac_atp(f, LT_SOCK_CONN, ATP_TREQ, 0x01, (uint16_t)now, 0x2C, PAP_TICKLE, NULL, 0);
            lt_node_rx(n, f, fl, now);
        }
        lt_node_tick(n, now);
    }
}

static void test_idle_hold(void)
{
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    uint8_t f[LLAP_MAX];
    for (int run = 0; run < 3; run++) {
        lt_node_t n;
        uint64_t t0 = elap_online(&n, 60000);
        job_eof = 0;
        job_normal = -1;
        size_t fl = emac_atp(f, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0700, 0x2C, PAP_OPEN_CONN, oc, sizeof oc);
        lt_node_rx(&n, f, fl, t0);
        CHECK(n.pap.open);
        uint64_t open_until = t0 + 59990, closed_at = t0 + 60000;
        if (run == 1) {                                   // paper out holds the clock
            elap_run(&n, t0, t0 + 10, t0 + 9990);
            lt_node_set_status(&n, LT_IW_PAPER_OUT);
            elap_run(&n, t0, t0 + 10000, t0 + 99990);
            lt_node_set_status(&n, 0);
            elap_run(&n, t0, t0 + 100000, t0 + 159980);
            CHECK(n.pap.open);
            reset_fake();
            elap_run(&n, t0, t0 + 159990, t0 + 160000);
            CHECK(!n.pap.open);
            CHECK_EQ_INT(close_conns_sent(), 1);
            CHECK_EQ_INT(job_eof, 1);
            continue;
        }
        if (run == 2) {                                   // data restarts it
            uint16_t tid = 0;
            for (int i = 0; i < nsent && i < MAXSENT; i++)
                if (eatp_of(i)[5] == PAP_SEND_DATA) tid = (uint16_t)(eatp_of(i)[2] << 8 | eatp_of(i)[3]);
            elap_run(&n, t0, t0 + 10, t0 + 29990);
            const uint8_t chunk[] = { 0x0D, 0x0A };
            fl = emac_atp(f, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid, 0x2C, PAP_DATA, chunk, sizeof chunk);
            lt_node_rx(&n, f, fl, t0 + 30000);
            CHECK_EQ_INT(n.pap.bytes, 2);
            elap_run(&n, t0, t0 + 30000, t0 + 89990);
            CHECK(n.pap.open);
            reset_fake();
            elap_run(&n, t0, t0 + 90000, t0 + 90000);
        } else {
            elap_run(&n, t0, t0 + 10, open_until);
            CHECK(n.pap.open);
            reset_fake();
            elap_run(&n, t0, closed_at, closed_at);
        }
        CHECK(!n.pap.open);
        CHECK_EQ_INT(close_conns_sent(), 1);
        CHECK_EQ_INT(job_eof, 1);
        // What came prints; a connection that sent nothing is no print at all.
        CHECK_EQ_INT(job_normal, run == 2 ? 1 : 0);
        CHECK(strstr(lastlog, "sent nothing") != NULL);
    }
}

// The board holding the Mac off (no read out: the spool has no room) is not
// the Mac being idle: the idle hold waits, however slow the printer drains.
static void test_idle_hold_waits_for_room(void)
{
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    uint8_t f[LLAP_MAX];
    lt_node_t n;
    uint64_t t0 = elap_online(&n, 60000);
    job_eof = 0;
    job_normal = -1;
    fake_room = (size_t)-1;
    size_t fl = emac_atp(f, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0900, 0x2C, PAP_OPEN_CONN, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t0);
    uint16_t tid = 0;
    for (int i = 0; i < nsent && i < MAXSENT; i++)
        if (eatp_of(i)[5] == PAP_SEND_DATA) tid = (uint16_t)(eatp_of(i)[2] << 8 | eatp_of(i)[3]);
    fake_room = 100;                                      // the printer is behind
    const uint8_t chunk[] = { 0x0D, 0x0A };
    fl = emac_atp(f, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid, 0x2C, PAP_DATA, chunk, sizeof chunk);
    lt_node_rx(&n, f, fl, t0 + 1000);
    CHECK_EQ_INT(n.pap.rd_want, 0);                       // no read out: no room
    elap_run(&n, t0, t0 + 1010, t0 + 180000);
    CHECK(n.pap.open);                                    // three minutes, still waiting
    CHECK_EQ_INT(job_eof, 0);
    fake_room = (size_t)-1;                               // the printer caught up
    elap_run(&n, t0, t0 + 180010, t0 + 180010);
    CHECK(n.pap.rd_want != 0);                            // the next read is out
    elap_run(&n, t0, t0 + 180020, t0 + 240000);
    CHECK(n.pap.open);
    elap_run(&n, t0, t0 + 240010, t0 + 240020);
    CHECK(!n.pap.open);                                   // and now the Mac is idle
    CHECK_EQ_INT(job_normal, 1);
}

static void test_cable_no_idle_hold(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 200);
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xF4, 1, 0, 0 };
    size_t fl = mac_atp(f, 200, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0800, 0x2D, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    for (uint64_t now = t + 10; now <= t + 60000; now += 10) lt_node_tick(&n, now);
    CHECK(n.pap.open);
    for (uint64_t now = t + 60010; now <= t + 119990; now += 10) lt_node_tick(&n, now);
    CHECK(n.pap.open);                                    // today's 120 s timer, no sooner
}

// W-1079: the Macs a link has heard lately, for the settings page's cards.
static void test_seen(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 150);
    uint8_t f[LLAP_MAX];
    const uint8_t echo[] = { 1, 'h', 'i' };
    size_t fl = mac_ddp(f, 150, SOCK_AEP, DDP_AEP, echo, sizeof echo);    // from node 83
    lt_node_rx(&n, f, fl, t);
    const uint8_t enq[] = { 99, 99, LLAP_ENQ };           // a node choosing its ID: not one yet
    lt_node_rx(&n, enq, sizeof enq, t + 1000);
    lt_seen_t s[LT_SEEN_SLOTS];
    size_t k = lt_node_seen(&n, t + 5000, 600000, s, LT_SEEN_SLOTS);
    CHECK_EQ_INT(k, 1);
    CHECK_EQ_INT(s[0].node, 83);
    CHECK_EQ_INT(s[0].ago_s, 5);
    CHECK_EQ_INT(lt_node_seen(&n, t + 600001, 600000, s, LT_SEEN_SLOTS), 0);   // quiet 10 min

    // More nodes than slots: the one quiet longest makes room. Newest first.
    for (int id = 1; id <= LT_SEEN_SLOTS + 1; id++) {
        const uint8_t rts[] = { 150, (uint8_t)id, LLAP_RTS };
        lt_node_rx(&n, rts, sizeof rts, t + 10000 + (uint64_t)id * 1000);
    }
    k = lt_node_seen(&n, t + 40000, 600000, s, LT_SEEN_SLOTS);
    CHECK_EQ_INT(k, LT_SEEN_SLOTS);
    CHECK_EQ_INT(s[0].node, LT_SEEN_SLOTS + 1);
    CHECK_EQ_INT(s[0].ago_s, 40 - 10 - (LT_SEEN_SLOTS + 1));
    int old = 0;
    for (size_t i = 0; i < k; i++) old |= s[i].node == 1 || s[i].node == 83;
    CHECK(!old);
    CHECK_EQ_INT(lt_node_seen(&n, t + 40000, 600000, s, 3), 3);             // as many as asked
    // Heard again: the same slot, now newest.
    const uint8_t again[] = { 150, 5, LLAP_RTS };
    lt_node_rx(&n, again, sizeof again, t + 41000);
    k = lt_node_seen(&n, t + 41000, 600000, s, LT_SEEN_SLOTS);
    CHECK_EQ_INT(k, LT_SEEN_SLOTS);
    CHECK_EQ_INT(s[0].node, 5);
    CHECK_EQ_INT(s[0].ago_s, 0);
}

// NBP names are MacRoman; the settings page wants UTF-8.
// ------------------------------------------------------------- W-1088: several names

static int po_calls, po_reg = -1, po_busy;
static int f_pap_open(void *ctx, int reg, uint64_t now) { (void)ctx; (void)now; po_calls++; po_reg = reg; return po_busy; }
static size_t f_reg_status(void *ctx, int reg, uint8_t *pstr, size_t cap)
{
    (void)ctx;
    int n = snprintf((char *)pstr + 1, cap - 1, "status: idle (reg %d)", reg);
    pstr[0] = (uint8_t)n;
    return (size_t)n + 1;
}
static const lt_ops_t OPS_REG = { NULL, f_send, f_claim, f_log, f_begin, f_data, f_room, f_end, NULL,
                                  f_pap_open, f_reg_status, NULL, NULL };

// A node's registrations are its link's (cfg.regs, in PSRAM on the board).
static lt_reg_t reg_room[4][LT_REG_MAX];
static int reg_next;

static uint64_t online_reg(lt_node_t *n, uint8_t id)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, id, 1, NULL, reg_room[reg_next++ % 4] };
    lt_node_init(n, &cfg, &OPS_REG, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(n, t);
    reset_fake();
    return t;
}

static const uint8_t *PS(const char *s)
{
    static uint8_t b[4][34];
    static int k;
    uint8_t *o = b[k++ % 4];
    o[0] = (uint8_t)strlen(s);
    memcpy(o + 1, s, o[0]);
    return o;
}

// A Mac's LkUp for obj:type@*, broadcast.
static void lkup(lt_node_t *n, const uint8_t *obj, const uint8_t *type, uint64_t t)
{
    uint8_t nbp[128], f[LLAP_MAX];
    size_t k = 0;
    nbp[k++] = 0x21; nbp[k++] = 0x2A;
    nbp[k++] = 0; nbp[k++] = 0; nbp[k++] = 83; nbp[k++] = 0xFB; nbp[k++] = 0;
    memcpy(nbp + k, obj, 1 + (size_t)obj[0]); k += 1 + obj[0];
    memcpy(nbp + k, type, 1 + (size_t)type[0]); k += 1 + type[0];
    nbp[k++] = 1; nbp[k++] = '*';
    lt_node_rx(n, f, mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k), t);
}

// The NBP tuple of reply i: its socket and object.
static uint8_t reply_sock(int i) { return sent[i][3 + 5 + 2 + 3]; }
static const uint8_t *reply_obj(int i) { return sent[i] + 3 + 5 + 2 + 5; }

static void test_register_two(void)
{
    lt_node_t n;
    uint64_t t = online_reg(&n, 200);
    CHECK_EQ_INT(lt_node_register(&n, 1, PS("Office"), PS("LaserWriter"), 0x85), 0);
    lkup(&n, PS("="), PS("LaserWriter"), t);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(reply_sock(0), 0x85);
    CHECK_EQ_MEM(reply_obj(0), PS("Office"), 7);
    reset_fake();
    lkup(&n, PS("="), PS("="), t + 1000);
    CHECK_EQ_INT(nsent, 2);                               // the ImageWriter, and Office
    reset_fake();
    lkup(&n, PS("="), PS("ImageWriter"), t + 2000);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(reply_sock(0), LT_SOCK_SLS);
    // Cleared: not answered any more.
    CHECK_EQ_INT(lt_node_register(&n, 1, NULL, NULL, 0), 0);
    reset_fake();
    lkup(&n, PS("="), PS("LaserWriter"), t + 3000);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(lt_node_register(&n, 0, PS("x"), PS("y"), 0x85), -1);   // 0 is the node's own
    CHECK_EQ_INT(lt_node_register(&n, LT_REG_MAX, PS("x"), PS("y"), 0x85), -1);
}

static void test_open_on_reg(void)
{
    lt_node_t n;
    uint64_t t = online_reg(&n, 200);
    lt_node_register(&n, 1, PS("Office"), PS("LaserWriter"), 0x85);
    po_calls = 0; po_busy = 0; job_begins = 0;
    uint8_t f[LLAP_MAX];
    // SendStatus at its socket: the registration's own status.
    size_t fl = mac_atp(f, 200, 0x85, ATP_TREQ, 0x01, 0x0301, 0, PAP_SEND_STATUS, 0, 0, NULL, 0);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(sent[0][3 + 3], 0x85);                   // from its socket
    CHECK(memmem(sent[0], sent_len[0], "status: idle (reg 1)", 20) != NULL);
    // OpenConn there asks pap_open, not the ImageWriter's job_begin.
    reset_fake();
    const uint8_t oc[] = { 0xFC, 8, 0, 0 };
    fl = mac_atp(f, 200, 0x85, ATP_TREQ | ATP_XO, 0x01, 0x0302, 0x51, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t + 10);
    CHECK_EQ_INT(po_calls, 1);
    CHECK_EQ_INT(po_reg, 1);
    CHECK_EQ_INT(job_begins, 0);
    CHECK(n.pap.open);
    CHECK_EQ_INT(n.pap.reg, 1);
    CHECK_EQ_INT(atp_of(0)[10], 0x00);                    // result OK
    CHECK_EQ_INT(atp_of(0)[11], 0x00);
    // Busy behind it.
    lt_node_t m;
    t = online_reg(&m, 201);
    lt_node_register(&m, 1, PS("Office"), PS("LaserWriter"), 0x85);
    po_busy = 1;
    fl = mac_atp(f, 201, 0x85, ATP_TREQ | ATP_XO, 0x01, 0x0303, 0x52, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&m, f, fl, t);
    CHECK(!m.pap.open);
    CHECK_EQ_INT(atp_of(0)[10], 0xFF);                    // PAP_RESULT_BUSY
    CHECK_EQ_INT(atp_of(0)[11], 0xFF);
    po_busy = 0;
}

static void test_marker(void)
{
    lt_node_t n;
    uint64_t t = online_reg(&n, 200);
    lt_node_register(&n, 4, PS("airwriter-2434"), PS("AirWriter"), 0x89);
    lkup(&n, PS("="), PS("AirWriter"), t);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(reply_sock(0), 0x89);
    // It never opens.
    reset_fake();
    po_calls = 0;
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xFC, 8, 0, 0 };
    size_t fl = mac_atp(f, 200, 0x89, ATP_TREQ | ATP_XO, 0x01, 0x0401, 0x53, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t + 5);
    CHECK_EQ_INT(po_calls, 0);
    CHECK(!n.pap.open);
}

// W-1095: a relayed connection carries any number of jobs, as a
// LaserWriter's does. The Mac's EOF ends one (job_end LT_JOB_EOF, not the
// once-only end); once the printer's EOF has answered the Mac's read, the Mac
// is asked for the next with a new SendData.
static int     jeofs;                      // job_end(LT_JOB_EOF) calls
static uint8_t ro[64], ro_last[64];        // what the printer has to say; the last answer
static size_t  ro_len, ro_last_n;
static int     ro_eof, ro_last_eof;
static void f_end_jobs(void *ctx, int normal)
{
    if (normal == LT_JOB_EOF) jeofs++;
    else f_end(ctx, normal);
}
static size_t f_reg_output(void *ctx, int reg, uint8_t *buf, size_t max, int *eof, int again)
{
    (void)ctx; (void)reg;
    if (!again) {
        ro_last_n = ro_len < max ? ro_len : max;
        memcpy(ro_last, ro, ro_last_n);
        ro_last_eof = ro_eof;
        ro_len = 0;
        ro_eof = 0;
    }
    memcpy(buf, ro_last, ro_last_n);
    *eof = ro_last_eof;
    return ro_last_n;
}
static const lt_ops_t OPS_RELAYED = { NULL, f_send, f_claim, f_log, f_begin, f_data, f_room,
                                      f_end_jobs, NULL, f_pap_open, f_reg_status, f_reg_output,
                                      NULL };

static uint64_t online_relayed(lt_node_t *n, uint8_t id)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, id, 1, NULL, reg_room[reg_next++ % 4] };
    lt_node_init(n, &cfg, &OPS_RELAYED, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(n, t);
    reset_fake();
    jeofs = 0; job_eof = 0; job_normal = -1; job_len = 0; po_busy = 0;
    ro_len = ro_last_n = 0; ro_eof = ro_last_eof = 0;
    return t;
}

static uint16_t tid_of(int i) { return (uint16_t)(atp_of(i)[2] << 8 | atp_of(i)[3]); }

static void test_pap_relayed_jobs(void)
{
    lt_node_t n;
    uint64_t t = online_relayed(&n, 200);
    lt_node_register(&n, 1, PS("Office"), PS("LaserWriter"), 0x85);
    uint8_t f[LLAP_MAX];
    const uint8_t oc[] = { 0xFC, 8, 0, 0 };
    size_t fl = mac_atp(f, 200, 0x85, ATP_TREQ | ATP_XO, 0x01, 0x0901, 0x61, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    CHECK(n.pap.open && n.pap.reg == 1);
    CHECK_EQ_INT(atp_of(1)[5], PAP_SEND_DATA);
    CHECK_EQ_INT(atp_of(1)[7], 1);
    uint16_t tid1 = tid_of(1);

    // Job 1, the query, with EOF: one job over, the connection open, no next read yet.
    reset_fake();
    const char q[] = "%!PS-Adobe-2.0 Query\r";
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid1, 0x61, PAP_DATA, 1, 0,
                 (const uint8_t *)q, 21);
    lt_node_rx(&n, f, fl, t + 100);
    CHECK_EQ_INT(nsent, 1);                               // the TRel alone
    CHECK_EQ_INT(atp_of(0)[0], ATP_TREL);
    CHECK_EQ_INT(jeofs, 1);
    CHECK_EQ_INT(job_eof, 0);
    CHECK_EQ_INT(job_len, 21);
    CHECK(n.pap.open);

    // The Mac reads the answer: held while the printer has said nothing.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0xFF, 0x0902, 0x61, PAP_SEND_DATA, 0, 1, NULL, 0);
    lt_node_rx(&n, f, fl, t + 110);
    CHECK_EQ_INT(nsent, 0);
    lt_node_tick(&n, t + 1000);
    CHECK_EQ_INT(nsent, 0);

    // The printer's answer and EOF reach that read; then a SendData for the next job.
    memcpy(ro, "true\r", 5);
    ro_len = 5; ro_eof = 1;
    lt_node_tick(&n, t + 1100);
    CHECK_EQ_INT(nsent, 2);
    CHECK_EQ_INT(atp_of(0)[5], PAP_DATA);
    CHECK_EQ_INT(atp_of(0)[6], 1);                        // EOF
    CHECK_EQ_INT(sent_len[0], 3 + 5 + 8 + 5);
    CHECK_EQ_INT(atp_of(1)[0], ATP_TREQ | ATP_XO);
    CHECK_EQ_INT(atp_of(1)[5], PAP_SEND_DATA);
    CHECK_EQ_INT(atp_of(1)[7], 2);                        // the next sequence number
    uint16_t tid2 = tid_of(1);
    CHECK(tid2 != tid1);

    // A PAP-level retransmit of that read: the same answer, and no third job.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0xFF, 0x0903, 0x61, PAP_SEND_DATA, 0, 1, NULL, 0);
    lt_node_rx(&n, f, fl, t + 1200);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(atp_of(0)[6], 1);
    CHECK_EQ_MEM(atp_of(0) + 8, "true\r", 5);

    // Job 2, the page, on the new read: its EOF ends it.
    reset_fake();
    uint8_t page[100];
    for (int i = 0; i < 100; i++) page[i] = (uint8_t)('a' + i % 26);
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid2, 0x61, PAP_DATA, 1, 0, page, sizeof page);
    lt_node_rx(&n, f, fl, t + 2000);
    CHECK_EQ_INT(jeofs, 2);
    CHECK_EQ_INT(job_len, 121);
    CHECK_EQ_MEM(job + 21, page, sizeof page);
    CHECK_EQ_INT(job_eof, 0);

    // Its EOF to the Mac's next read, then the read for job 3.
    reset_fake();
    ro_eof = 1;
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0xFF, 0x0904, 0x61, PAP_SEND_DATA, 0, 2, NULL, 0);
    lt_node_rx(&n, f, fl, t + 2100);
    CHECK_EQ_INT(nsent, 2);
    CHECK_EQ_INT(atp_of(0)[6], 1);
    CHECK_EQ_INT(atp_of(1)[5], PAP_SEND_DATA);
    CHECK_EQ_INT(atp_of(1)[7], 3);
    CHECK(strstr(lastlog, "job 2") != NULL);

    // CloseConn ends the connection, once.
    reset_fake();
    fl = mac_atp(f, 200, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0x01, 0x0905, 0x61, PAP_CLOSE_CONN, 0, 0, NULL, 0);
    lt_node_rx(&n, f, fl, t + 2200);
    CHECK(!n.pap.open);
    CHECK_EQ_INT(job_eof, 1);
    CHECK_EQ_INT(job_normal, 1);
    CHECK_EQ_INT(jeofs, 2);

    // The node's own registration, the ImageWriter's, still ends its
    // connection at the Mac's EOF: no next read, however long.
    t = online_relayed(&n, 201);
    fl = mac_atp(f, 201, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 0x0911, 0x62, PAP_OPEN_CONN, 0, 0, oc, sizeof oc);
    lt_node_rx(&n, f, fl, t);
    CHECK(n.pap.open && n.pap.reg == 0);
    tid1 = tid_of(1);
    reset_fake();
    fl = mac_atp(f, 201, LT_SOCK_CONN, ATP_TRESP | ATP_EOM, 0, tid1, 0x62, PAP_DATA, 1, 0, page, sizeof page);
    lt_node_rx(&n, f, fl, t + 100);
    CHECK_EQ_INT(job_eof, 1);                             // job_end at EOF, as before
    CHECK_EQ_INT(job_normal, 1);
    CHECK_EQ_INT(jeofs, 0);
    fl = mac_atp(f, 201, LT_SOCK_CONN, ATP_TREQ | ATP_XO, 0xFF, 0x0912, 0x62, PAP_SEND_DATA, 0, 1, NULL, 0);
    lt_node_rx(&n, f, fl, t + 200);
    reset_fake();
    for (uint64_t k = 300; k <= 30000; k += 500) lt_node_tick(&n, t + k);
    int reads = 0;
    for (int i = 0; i < nsent && i < MAXSENT; i++)
        reads += atp_of(i)[0] == (ATP_TREQ | ATP_XO) && atp_of(i)[5] == PAP_SEND_DATA;
    CHECK_EQ_INT(reads, 0);
    CHECK(n.pap.open);
}

// W-1088 review I5: there are four nodes on the board (in PSRAM since
// W-1094), so their registrations are the link's room, not each node's. A
// node given none answers only as itself.
static void test_register_needs_room(void)
{
    CHECK(sizeof(lt_node_t) <= 6160);                     // 6072 before W-1088, 6152 with W-1098 (host)
    lt_node_t n;
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 200, 1, NULL, NULL };
    lt_node_init(&n, &cfg, &OPS_REG, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&n, t);
    reset_fake();
    CHECK_EQ_INT(lt_node_register(&n, 1, PS("Office"), PS("LaserWriter"), 0x85), -1);
    lt_offer_t rel = { {0}, {0} };
    memcpy(rel.obj, PS("Office"), 7);
    memcpy(rel.type, PS("LaserWriter"), 12);
    lt_node_offer(&n, PS("ImageWriter II"), &rel, 1, PS("airwriter-2434"));
    lkup(&n, PS("="), PS("="), t);
    CHECK_EQ_INT(nsent, 1);                               // itself only
    CHECK_EQ_INT(reply_sock(0), LT_SOCK_SLS);
    // Given room, the same offer answers for all three.
    lt_node_t m;
    t = online_reg(&m, 201);
    lt_node_offer(&m, PS("ImageWriter II"), &rel, 1, PS("airwriter-2434"));
    lkup(&m, PS("="), PS("="), t);
    CHECK_EQ_INT(nsent, 3);
}

static void test_register_macroman(void)
{
    lt_node_t n;
    uint64_t t = online_reg(&n, 200);
    static const uint8_t cafe[] = { 4, 'C', 'a', 'f', 0x8E };
    static const uint8_t cafe_lc[] = { 4, 'c', 'a', 'f', 0x8E };
    lt_node_register(&n, 2, cafe, PS("LaserWriter"), 0x86);
    lkup(&n, cafe_lc, PS("LaserWriter"), t);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_MEM(reply_obj(0), cafe, sizeof cafe);
}

static void test_nbp_utf8(void)
{
    char out[3 * NBP_NAME_MAX + 1];
    const uint8_t mac[] = { 13, 'W','e','l','l','s',0xD5,'s',' ','S','E','/','3','0' };
    CHECK_EQ_INT(nbp_utf8(out, sizeof out, mac), 15);
    CHECK(!strcmp(out, "Wells\xE2\x80\x99s SE/30"));
    const uint8_t odd[] = { 5, 0x80, 0xF0, 0x07, 0x7F, 'x' };   // Ä, the Apple logo, controls
    nbp_utf8(out, sizeof out, odd);
    CHECK(!strcmp(out, "\xC3\x84\xEF\xA3\xBF?" "?x"));
    CHECK_EQ_INT(nbp_utf8(out, 9, mac), 8);                   // cut at a whole character
    CHECK(!strcmp(out, "Wells\xE2\x80\x99"));
    CHECK_EQ_INT(nbp_utf8(out, 8, mac), 5);
    CHECK(!strcmp(out, "Wells"));
    const uint8_t empty[] = { 0 };
    CHECK_EQ_INT(nbp_utf8(out, sizeof out, empty), 0);
    CHECK_EQ_INT(out[0], 0);

    // All of 0x80-0xFF, as Python's mac_roman codec has them.
    static const char *const want[4] = {
        "\xC3\x84\xC3\x85\xC3\x87\xC3\x89\xC3\x91\xC3\x96\xC3\x9C\xC3\xA1\xC3\xA0\xC3\xA2\xC3\xA4\xC3\xA3\xC3\xA5\xC3\xA7\xC3\xA9\xC3\xA8\xC3\xAA\xC3\xAB\xC3\xAD\xC3\xAC\xC3\xAE\xC3\xAF\xC3\xB1\xC3\xB3\xC3\xB2\xC3\xB4\xC3\xB6\xC3\xB5\xC3\xBA\xC3\xB9\xC3\xBB\xC3\xBC",
        "\xE2\x80\xA0\xC2\xB0\xC2\xA2\xC2\xA3\xC2\xA7\xE2\x80\xA2\xC2\xB6\xC3\x9F\xC2\xAE\xC2\xA9\xE2\x84\xA2\xC2\xB4\xC2\xA8\xE2\x89\xA0\xC3\x86\xC3\x98\xE2\x88\x9E\xC2\xB1\xE2\x89\xA4\xE2\x89\xA5\xC2\xA5\xC2\xB5\xE2\x88\x82\xE2\x88\x91\xE2\x88\x8F\xCF\x80\xE2\x88\xAB\xC2\xAA\xC2\xBA\xCE\xA9\xC3\xA6\xC3\xB8",
        "\xC2\xBF\xC2\xA1\xC2\xAC\xE2\x88\x9A\xC6\x92\xE2\x89\x88\xE2\x88\x86\xC2\xAB\xC2\xBB\xE2\x80\xA6\xC2\xA0\xC3\x80\xC3\x83\xC3\x95\xC5\x92\xC5\x93\xE2\x80\x93\xE2\x80\x94\xE2\x80\x9C\xE2\x80\x9D\xE2\x80\x98\xE2\x80\x99\xC3\xB7\xE2\x97\x8A\xC3\xBF\xC5\xB8\xE2\x81\x84\xE2\x82\xAC\xE2\x80\xB9\xE2\x80\xBA\xEF\xAC\x81\xEF\xAC\x82",
        "\xE2\x80\xA1\xC2\xB7\xE2\x80\x9A\xE2\x80\x9E\xE2\x80\xB0\xC3\x82\xC3\x8A\xC3\x81\xC3\x8B\xC3\x88\xC3\x8D\xC3\x8E\xC3\x8F\xC3\x8C\xC3\x93\xC3\x94\xEF\xA3\xBF\xC3\x92\xC3\x9A\xC3\x9B\xC3\x99\xC4\xB1\xCB\x86\xCB\x9C\xC2\xAF\xCB\x98\xCB\x99\xCB\x9A\xC2\xB8\xCB\x9D\xCB\x9B\xCB\x87",
    };
    for (int c = 0; c < 4; c++) {
        uint8_t name[1 + 32] = { 32 };
        for (int i = 0; i < 32; i++) name[1 + i] = (uint8_t)(0x80 + 32 * c + i);
        CHECK_EQ_INT(nbp_utf8(out, sizeof out, name), strlen(want[c]));
        CHECK(!strcmp(out, want[c]));
    }
}

// Macintosh names: System 7 and later register the name from Sharing Setup
// with NBP as <name>:Workstation. A node given room for names asks each node
// it hears, straight to that node's NIS.
static lt_name_t names[LT_SEEN_SLOTS];
static const uint8_t SE30[] = { 13, 'W','e','l','l','s',0xD5,'s',' ','S','E','/','3','0' };

static uint64_t online_named(lt_node_t *n, uint8_t id)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, id, 1, names, NULL };
    lt_node_init(n, &cfg, &OPS, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(n, t);
    reset_fake();
    return t;
}

// Sent frame i is a LkUp for =:Workstation@* to node's NIS, its replies to ours.
static int is_name_ask(int i, uint8_t node, uint8_t me)
{
    ddp_t d;
    nbp_t q;
    uint8_t ws[NBP_NAME_MAX + 1];
    nbp_name(ws, "Workstation");
    if (i >= nsent || i >= MAXSENT || ddp_parse(sent[i], sent_len[i], &d) || d.type != DDP_NBP ||
        nbp_parse(d.data, d.len, &q))
        return 0;
    return sent[i][0] == node && d.dst.node == node && d.dst.sock == SOCK_NIS &&
           d.src.sock == SOCK_NIS && q.fn == NBP_LKUP && q.count == 1 &&
           q.t.addr.node == me && q.t.addr.sock == SOCK_NIS && q.t.obj[0] == 1 &&
           q.t.obj[1] == '=' && !memcmp(q.t.type, ws, 1 + ws[0]) && q.t.zone[0] == 1 &&
           q.t.zone[1] == '*';
}

// A LkUp-Reply from `from` (its entity on the echo socket, as System 7 has it).
static size_t name_reply(uint8_t *f, uint8_t from, uint8_t to, const char *type,
                         const uint8_t *name)
{
    nbp_tuple_t t = { .addr = { 0, from, SOCK_AEP }, .enumerator = 0 };
    memcpy(t.obj, name, 1 + (size_t)name[0]);
    nbp_name(t.type, type);
    t.zone[0] = 1;
    t.zone[1] = '*';
    uint8_t nbp[2 + 5 + 3 * (NBP_NAME_MAX + 1)];
    size_t k = nbp_build(nbp, sizeof nbp, NBP_LKUP_REPLY, 1, &t);
    lt_addr_t dst = { 0, to, SOCK_NIS }, src = { 0, from, SOCK_NIS };
    return ddp_build(f, LLAP_MAX, to, from, 0, &dst, &src, DDP_NBP, nbp, k);
}

static const uint8_t *seen_name(const lt_node_t *n, uint8_t node, uint64_t now)
{
    lt_seen_t s[LT_SEEN_SLOTS];
    size_t k = lt_node_seen(n, now, LT_SEEN_MS, s, LT_SEEN_SLOTS);
    for (size_t i = 0; i < k; i++)
        if (s[i].node == node) return s[i].name;
    return NULL;
}

static void test_name_asked(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);                    // node 83 heard
    lt_node_tick(&n, t + LT_NAME_DELAY_MS - 1);
    CHECK_EQ_INT(nsent, 0);                               // a moment's grace first
    uint64_t at = t + LT_NAME_DELAY_MS;
    lt_node_tick(&n, at);
    CHECK_EQ_INT(nsent, 1);
    CHECK(is_name_ask(0, 83, 150));
    CHECK_EQ_INT(sent[0][2], LLAP_SHORT);

    // Unanswered: asked again, each wait twice the last, from LT_NAME_RETRY_MS
    // up to LT_NAME_REFRESH_MS, while the node is still heard.
    uint32_t gap = LT_NAME_RETRY_MS;
    int k = 0;
    for (int i = 1; i <= 10; i++) {
        uint64_t next = at + gap;
        for (uint64_t s = at + 1000; s < next; s += 1000) {
            if (!(++k % 60)) lt_node_rx(&n, rts, sizeof rts, s);
            lt_node_tick(&n, s);
        }
        CHECK_EQ_INT(nsent, i);
        lt_node_tick(&n, next);
        CHECK_EQ_INT(nsent, i + 1);
        CHECK(is_name_ask(i, 83, 150));
        at = next;
        gap = 2 * gap < LT_NAME_REFRESH_MS ? 2 * gap : LT_NAME_REFRESH_MS;
    }
    CHECK(seen_name(&n, 83, at) == NULL);
}

// A Mac at node 83 whose name is registered from `named_at` on, answering the
// node's asks from then, run from..to in 100 ms ticks. When the node first
// knows the name, or 0.
static uint64_t mac_named_from(lt_node_t *n, uint64_t from, uint64_t to, uint64_t named_at)
{
    uint8_t f[LLAP_MAX];
    reset_fake();
    int done = 0;
    for (uint64_t s = from; s <= to; s += 100) {
        lt_node_tick(n, s);
        for (; done < nsent && done < MAXSENT; done++) {
            if (!is_name_ask(done, 83, 150) || s < named_at) continue;
            size_t fl = name_reply(f, 83, 150, "Workstation", SE30);
            lt_node_rx(n, f, fl, s + 20);
        }
        if (seen_name(n, 83, s + 20)) return s;
    }
    return 0;
}

// On the cable a Mac opens AppleTalk early in its startup and registers its
// name much later (the SE/30, 2026-10-20): the asks must outlast that.
static void test_name_late_mac(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);
    uint64_t got = mac_named_from(&n, t, t + 300000, t + 60000);
    CHECK(got >= t + 60000);
    CHECK(got && got < t + 60000 + 2 * 40000);           // within the wait then due
    CHECK(nsent <= 8);                                    // and few asks for it
}

// Registering its name, a Mac looks that name up first (=its name:Workstation@*,
// a few times a second apart): the node asks it a moment after the last.
static void test_name_after_registration(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);
    CHECK_EQ_INT(mac_named_from(&n, t, t + 40000, (uint64_t)-1), 0);

    uint8_t nbp[2 + 5 + 3 * (NBP_NAME_MAX + 1)], f[LLAP_MAX];
    nbp_tuple_t q = { .addr = { 0, 83, 0xFB }, .enumerator = 0 };
    memcpy(q.obj, SE30, sizeof SE30);
    nbp_name(q.type, "Workstation");
    q.zone[0] = 1;
    q.zone[1] = '*';
    size_t k = nbp_build(nbp, sizeof nbp, NBP_LKUP, 7, &q);
    size_t fl = mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    for (uint64_t s = t + 40000; s < t + 43000; s += 100) {
        if (!((s - t) % 1000)) lt_node_rx(&n, f, fl, s);    // at 40, 41 and 42 s
        lt_node_tick(&n, s);
    }
    uint64_t got = mac_named_from(&n, t + 43000, t + 60000, t + 43000);
    CHECK(got && got <= t + 43000 + LT_NAME_RETRY_MS);
}

static void test_name_answered(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    uint8_t f[LLAP_MAX];
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);
    uint64_t at = t + LT_NAME_DELAY_MS;
    lt_node_tick(&n, at);
    CHECK_EQ_INT(nsent, 1);
    CHECK(seen_name(&n, 83, at) == NULL);

    size_t fl = name_reply(f, 83, 150, "Workstation", SE30);
    lt_node_rx(&n, f, fl, at + 20);
    const uint8_t *name = seen_name(&n, 83, at + 20);
    CHECK(name != NULL);
    if (name) CHECK_EQ_MEM(name, SE30, sizeof SE30);
    CHECK(strstr(lastlog, "node 83 is Wells\xE2\x80\x99s SE/30") != NULL);
    CHECK_EQ_INT(nsent, 1);                               // a reply is not answered

    // Answered: no retries, only the refresh.
    int k = 0;
    for (uint64_t s = at + 1000; s < at + 20 + LT_NAME_REFRESH_MS; s += 1000, k++) {
        if (!(k % 60)) lt_node_rx(&n, rts, sizeof rts, s);
        lt_node_tick(&n, s);
    }
    CHECK_EQ_INT(nsent, 1);
    lt_node_tick(&n, at + 20 + LT_NAME_REFRESH_MS);
    CHECK_EQ_INT(nsent, 2);
    CHECK(is_name_ask(1, 83, 150));
    name = seen_name(&n, 83, at + 20 + LT_NAME_REFRESH_MS);
    CHECK(name != NULL);                                  // kept while the refresh is out

    // A refresh no one answers, LT_NAME_TRIES asks running: the name goes
    // when the last one's wait is out (the ID may be another Mac's now).
    uint64_t r = at + 20 + LT_NAME_REFRESH_MS;
    uint64_t gone = r + (uint64_t)LT_NAME_RETRY_MS * ((1u << LT_NAME_TRIES) - 1);
    lt_node_rx(&n, rts, sizeof rts, r);
    for (uint64_t s = r + 1000; s < gone; s += 1000) lt_node_tick(&n, s);
    CHECK_EQ_INT(nsent, 1 + LT_NAME_TRIES);
    CHECK(seen_name(&n, 83, gone - 1000) != NULL);
    lt_node_tick(&n, gone);
    CHECK(seen_name(&n, 83, gone) == NULL);
}

static void test_name_other_replies(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    uint8_t f[LLAP_MAX];
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);
    lt_node_tick(&n, t + LT_NAME_DELAY_MS);

    // Another type is not the Macintosh name (its file server, say).
    size_t fl = name_reply(f, 83, 150, "AFPServer", SE30);
    lt_node_rx(&n, f, fl, t + 1100);
    CHECK(seen_name(&n, 83, t + 1100) == NULL);
    // The type is matched as NBP matches, case aside.
    fl = name_reply(f, 83, 150, "WORKSTATION", SE30);
    lt_node_rx(&n, f, fl, t + 1200);
    CHECK(seen_name(&n, 83, t + 1200) != NULL);
    // A reply to another node is not ours.
    const uint8_t other[] = { 4, 'I', 'I', 'c', 'i' };
    fl = name_reply(f, 84, 151, "Workstation", other);
    lt_node_rx(&n, f, fl, t + 1300);
    CHECK(seen_name(&n, 84, t + 1300) == NULL);
}

static void test_name_back_after_quiet(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    uint8_t f[LLAP_MAX];
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);
    lt_node_tick(&n, t + LT_NAME_DELAY_MS);
    size_t fl = name_reply(f, 83, 150, "Workstation", SE30);
    lt_node_rx(&n, f, fl, t + LT_NAME_DELAY_MS + 20);
    CHECK(seen_name(&n, 83, t + 2000) != NULL);

    // Quiet past the list's 10 minutes: no asks for a node not there, and
    // back, it is someone new until it says otherwise.
    uint64_t back = t + LT_NAME_DELAY_MS + 20 + LT_SEEN_MS + 1000;
    for (uint64_t s = t + 2000; s < back; s += 1000) lt_node_tick(&n, s);
    reset_fake();
    lt_node_rx(&n, rts, sizeof rts, back);
    CHECK(seen_name(&n, 83, back) == NULL);
    lt_node_tick(&n, back + LT_NAME_DELAY_MS);
    CHECK_EQ_INT(nsent, 1);
    CHECK(is_name_ask(0, 83, 150));
}

static void test_name_paced(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 150);
    for (uint8_t id = 80; id < 84; id++) {
        const uint8_t rts[] = { 150, id, LLAP_RTS };
        lt_node_rx(&n, rts, sizeof rts, t);
    }
    uint64_t at = t + LT_NAME_DELAY_MS;
    for (uint64_t s = at; s < at + LT_NAME_GAP_MS; s += 10) lt_node_tick(&n, s);
    CHECK_EQ_INT(nsent, 1);                               // one ask at a time
    for (uint64_t s = at + LT_NAME_GAP_MS; s < at + 4 * LT_NAME_GAP_MS; s += 10) lt_node_tick(&n, s);
    CHECK_EQ_INT(nsent, 4);
    int asked = 0;                                        // each node once
    for (int i = 0; i < 4; i++)
        for (uint8_t id = 80; id < 84; id++)
            if (is_name_ask(i, id, 150)) asked |= 1 << (id - 80);
    CHECK_EQ_INT(asked, 0xF);
}

static void test_names_off(void)
{
    lt_node_t n;
    uint64_t t = online(&n, 150);                         // no room for names given
    const uint8_t rts[] = { 150, 83, LLAP_RTS };
    lt_node_rx(&n, rts, sizeof rts, t);
    for (uint64_t s = t; s < t + 60000; s += 100) lt_node_tick(&n, s);
    CHECK_EQ_INT(nsent, 0);
    CHECK(seen_name(&n, 83, t + 60000) == NULL);
}

// Printers on the cable are heard all day (answers to the board's lookups,
// status, queries), but they are not Macs: whoever answers one of the
// board's printer lookups (lt_node_lookup) leaves the list, unless it gives
// a Macintosh name too (a Mac sharing a printer is still a Mac).
static int listed(const lt_node_t *n, uint8_t node, uint64_t now)
{
    lt_seen_t s[LT_SEEN_SLOTS];
    size_t k = lt_node_seen(n, now, LT_SEEN_MS, s, LT_SEEN_SLOTS);
    for (size_t i = 0; i < k; i++)
        if (s[i].node == node) return 1;
    return 0;
}

static int printer_bit(const lt_node_t *n, uint8_t node)
{
    return !!(n->printers[node / 8] & (1u << (node % 8)));
}

static void test_printer_not_a_mac(void)
{
    lt_node_t n;
    uint64_t t = online_named(&n, 184);
    uint8_t f[LLAP_MAX];
    static const uint8_t NTR[] = { 24, 'P','e','r','s','o','n','a','l',' ','L','a','s','e','r',
                                   'W','r','i','t','e','r',' ','N','T','R' };
    const uint8_t rts83[] = { 184, 83, LLAP_RTS }, rts233[] = { 184, 233, LLAP_RTS };
    lt_node_rx(&n, rts83, sizeof rts83, t);              // a Mac
    size_t fl = name_reply(f, 233, 184, "LaserWriter", NTR);
    lt_node_rx(&n, f, fl, t + 100);                       // the printer answers a lookup
    CHECK(listed(&n, 83, t + 200));
    CHECK(!listed(&n, 233, t + 200));
    CHECK(printer_bit(&n, 233));                          // and "a Mac heard" stays dark
    CHECK(!printer_bit(&n, 83));

    // It goes on talking: still not a Mac. Switched off past the list's 10
    // minutes and on again, it is the same printer at the same ID.
    lt_node_rx(&n, rts233, sizeof rts233, t + 5000);
    CHECK(!listed(&n, 233, t + 5000));
    uint64_t back = t + 5000 + LT_SEEN_MS + 1000;
    lt_node_rx(&n, rts233, sizeof rts233, back);
    CHECK(!listed(&n, 233, back));

    // Another AirWriter answers its marker the same way.
    static const uint8_t AW[] = { 15, 'i','m','a','g','e','w','r','i','t','e','r','-','2','4','3' };
    fl = name_reply(f, 200, 184, "AirWriter", AW);
    lt_node_rx(&n, f, fl, back + 100);
    CHECK(!listed(&n, 200, back + 100));

    // A Mac that answers a printer lookup (it shares one) and gives its
    // Macintosh name is a Mac.
    fl = name_reply(f, 83, 184, "LaserWriter", SE30);
    lt_node_rx(&n, f, fl, back + 200);
    CHECK(!listed(&n, 83, back + 200));
    fl = name_reply(f, 83, 184, "Workstation", SE30);
    lt_node_rx(&n, f, fl, back + 300);
    CHECK(listed(&n, 83, back + 300));

    // An answer a router relays from a printer beyond it (net 5, node 12)
    // marks neither the router nor node 12 on this cable.
    const uint8_t rts12[] = { 184, 12, LLAP_RTS };
    lt_node_rx(&n, rts12, sizeof rts12, back + 400);
    nbp_tuple_t r = { .addr = { 5, 12, 0x81 }, .enumerator = 0 };
    memcpy(r.obj, NTR, sizeof NTR);
    nbp_name(r.type, "LaserWriter");
    r.zone[0] = 1;
    r.zone[1] = '*';
    uint8_t nbp[2 + 5 + 3 * (NBP_NAME_MAX + 1)];
    size_t k = nbp_build(nbp, sizeof nbp, NBP_LKUP_REPLY, 1, &r);
    lt_addr_t dst = { 0, 184, SOCK_NIS }, src = { 5, 12, SOCK_NIS };
    fl = ddp_build(f, LLAP_MAX, 184, 254, 1, &dst, &src, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, back + 500);
    CHECK(listed(&n, 12, back + 500));
    CHECK(listed(&n, 254, back + 500));
    CHECK(!printer_bit(&n, 12));
    CHECK(!printer_bit(&n, 254));
}


// ------------------------------------------------------------- W-1082

// A node can run with no ImageWriter on its link: it answers neither the
// ImageWriter's lookups nor its PAP socket, and still echoes and keeps its ID.
static void test_serve_off(void)
{
    lt_node_t n;
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 201, 1, NULL, NULL };
    lt_node_init(&n, &cfg, &OPS, 0);
    lt_node_set_serve(&n, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&n, t);
    CHECK_EQ_INT(claimed, 201);
    CHECK(n.phase == LT_ONLINE);
    reset_fake();

    uint8_t nbp[64], f[LLAP_MAX];
    size_t k = 0;
    nbp[k++] = 0x21; nbp[k++] = 0x1E;
    nbp[k++] = 0; nbp[k++] = 0; nbp[k++] = 83; nbp[k++] = 0xFB; nbp[k++] = 0;
    nbp[k++] = 1; nbp[k++] = '=';
    nbp[k++] = 11; memcpy(nbp + k, "ImageWriter", 11); k += 11;
    nbp[k++] = 1; nbp[k++] = '*';
    size_t fl = mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 0);

    fl = mac_atp(f, 201, LT_SOCK_SLS, ATP_TREQ, 0x01, 7, 0, PAP_SEND_STATUS, 0, 0, NULL, 0);
    lt_node_rx(&n, f, fl, t);
    const uint8_t open[] = { 0xFB, 8, 0, 0 };
    int begins = job_begins;
    fl = mac_atp(f, 201, LT_SOCK_SLS, ATP_TREQ | ATP_XO, 0x01, 8, 0x42, PAP_OPEN_CONN, 0, 0,
                 open, sizeof open);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(job_begins, begins);
    CHECK(!n.pap.open);

    const uint8_t echo[] = { 1, 'h', 'i' };
    fl = mac_ddp(f, 201, SOCK_AEP, DDP_AEP, echo, sizeof echo);
    lt_node_rx(&n, f, fl, t);
    CHECK_EQ_INT(nsent, 1);

    // Shared again: the lookup is answered.
    lt_node_set_serve(&n, 1);
    fl = mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
    lt_node_rx(&n, f, fl, t + 10);
    CHECK_EQ_INT(nsent, 2);
}

static nbp_tuple_t found_t[4];
static lt_route_t  found_r[4];
static int         nfound;
static void f_found(void *ctx, const nbp_tuple_t *t, const lt_route_t *r)
{
    (void)ctx;
    if (nfound < 4) { found_t[nfound] = *t; found_r[nfound] = *r; }
    nfound++;
}

// A lookup of every LaserWriter goes to every node; answers reach `found`.
static void test_lookup_out(void)
{
    lt_ops_t ops = OPS;
    ops.found = f_found;
    lt_node_t n;
    static lt_name_t names[LT_SEEN_SLOTS];
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 202, 1, names, NULL };
    lt_node_init(&n, &cfg, &ops, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&n, t);
    reset_fake();
    nfound = 0;

    lt_node_lookup(&n, "LaserWriter", t);
    CHECK_EQ_INT(nsent, 1);
    ddp_t d;
    CHECK_EQ_INT(ddp_parse(sent[0], sent_len[0], &d), 0);
    CHECK_EQ_INT(sent[0][0], LLAP_BCAST);
    CHECK_EQ_INT(sent[0][2], LLAP_SHORT);
    CHECK_EQ_INT(d.dst.node, LLAP_BCAST);
    CHECK_EQ_INT(d.dst.sock, SOCK_NIS);
    CHECK_EQ_INT(d.src.sock, SOCK_NIS);
    CHECK_EQ_INT(d.type, DDP_NBP);
    nbp_t q;
    CHECK_EQ_INT(nbp_parse(d.data, d.len, &q), 0);
    CHECK_EQ_INT(q.fn, NBP_LKUP);
    CHECK_EQ_INT(q.count, 1);
    CHECK_EQ_INT(q.t.addr.node, 202);
    CHECK_EQ_INT(q.t.addr.sock, SOCK_NIS);
    CHECK(q.t.obj[0] == 1 && q.t.obj[1] == '=');
    CHECK(q.t.type[0] == 11 && !memcmp(q.t.type + 1, "LaserWriter", 11));
    CHECK(q.t.zone[0] == 1 && q.t.zone[1] == '*');

    // The LaserWriter at node 12 answers from its NIS with its listening socket.
    nbp_tuple_t lw = { .addr = { 0, 12, 0x85 }, .enumerator = 0 };
    nbp_name(lw.obj, "LaserWriter II NT");
    nbp_name(lw.type, "LaserWriter");
    lw.zone[0] = 1; lw.zone[1] = '*';
    uint8_t pkt[2 + 5 + 3 * (NBP_NAME_MAX + 1)], f[LLAP_MAX];
    size_t pl = nbp_build(pkt, sizeof pkt, NBP_LKUP_REPLY, q.id, &lw);
    lt_addr_t to = { 0, 202, SOCK_NIS }, from = { 0, 12, SOCK_NIS };
    size_t fl = ddp_build(f, sizeof f, 202, 12, 0, &to, &from, DDP_NBP, pkt, pl);
    lt_node_rx(&n, f, fl, t + 30);
    CHECK_EQ_INT(nfound, 1);
    CHECK_EQ_INT(found_t[0].addr.node, 12);
    CHECK_EQ_INT(found_t[0].addr.sock, 0x85);
    CHECK(found_t[0].obj[0] == 17 && !memcmp(found_t[0].obj + 1, "LaserWriter II NT", 17));
    CHECK_EQ_INT(found_r[0].via, 12);
    CHECK_EQ_INT(found_r[0].is_long, 0);
    CHECK_EQ_INT(found_r[0].addr.sock, 0x85);

    // A Macintosh name's answer is the name table's, not `found`'s.
    nbp_tuple_t ws = { .addr = { 0, 12, 0xFC }, .enumerator = 0 };
    nbp_name(ws.obj, "SE30");
    nbp_name(ws.type, "Workstation");
    ws.zone[0] = 1; ws.zone[1] = '*';
    pl = nbp_build(pkt, sizeof pkt, NBP_LKUP_REPLY, 9, &ws);
    fl = ddp_build(f, sizeof f, 202, 12, 0, &to, &from, DDP_NBP, pkt, pl);
    lt_node_rx(&n, f, fl, t + 40);
    CHECK_EQ_INT(nfound, 1);

    // The client's sockets and sends: a TReq from 0x83, a fresh TID each.
    uint16_t tid0 = lt_node_next_tid(&n), tid1 = lt_node_next_tid(&n);
    CHECK(tid1 == (uint16_t)(tid0 + 1));
    reset_fake();
    uint8_t user[4] = { 0x42, PAP_SEND_STATUS, 0, 0 };
    lt_node_send_atp(&n, &found_r[0], LT_SOCK_PAPC_ST, ATP_TREQ, 0x01, tid1, user, NULL, 0);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(ddp_parse(sent[0], sent_len[0], &d), 0);
    CHECK_EQ_INT(d.dst.node, 12);
    CHECK_EQ_INT(d.dst.sock, 0x85);
    CHECK_EQ_INT(d.src.sock, LT_SOCK_PAPC_ST);
    CHECK_EQ_INT(d.type, DDP_ATP);
}

static nbp_tuple_t checked[4];
static int         nchecked;
static void f_check(void *ctx, const nbp_tuple_t *t)
{
    (void)ctx;
    if (nchecked < 4) checked[nchecked] = *t;
    nchecked++;
}

// A LkUp from the Mac at node 83 for obj:type@*, broadcast.
static size_t lkup_from_83(uint8_t *f, const uint8_t *obj, const char *type)
{
    nbp_tuple_t q = { .addr = { 0, 83, SOCK_NIS }, .enumerator = 0 };
    memcpy(q.obj, obj, 1 + (size_t)obj[0]);
    nbp_name(q.type, type);
    q.zone[0] = 1; q.zone[1] = '*';
    uint8_t nbp[2 + 5 + 3 * (NBP_NAME_MAX + 1)];
    size_t k = nbp_build(nbp, sizeof nbp, NBP_LKUP, 3, &q);
    return mac_ddp(f, 0xFF, SOCK_NIS, DDP_NBP, nbp, k);
}

// W-1098: a node looking up one name, no wildcard in it, reaches name_check:
// that is how a printer switched on checks no one has its name. A Chooser's
// =:LaserWriter@* does not, nor a name with a wildcard in it.
static void test_name_check(void)
{
    lt_ops_t ops = OPS;
    ops.name_check = f_check;
    lt_node_t n;
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0x0000, 184, 1, NULL, NULL };
    lt_node_init(&n, &cfg, &ops, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&n, t);
    nchecked = 0;

    uint8_t f[LLAP_MAX], obj[NBP_NAME_MAX + 1];
    nbp_name(obj, "Personal LaserWriter NTR");
    lt_node_rx(&n, f, lkup_from_83(f, obj, "LaserWriter"), t);
    CHECK_EQ_INT(nchecked, 1);
    CHECK(!memcmp(checked[0].obj, obj, 1 + (size_t)obj[0]));
    CHECK(checked[0].type[0] == 11 && !memcmp(checked[0].type + 1, "LaserWriter", 11));
    CHECK_EQ_INT(checked[0].addr.node, 83);

    nbp_name(obj, "=");
    lt_node_rx(&n, f, lkup_from_83(f, obj, "LaserWriter"), t + 10);
    const uint8_t part[] = { 6, 'L', 'a', 's', 'e', 'r', NBP_WILD_PART };
    lt_node_rx(&n, f, lkup_from_83(f, part, "LaserWriter"), t + 20);
    CHECK_EQ_INT(nchecked, 1);

    // Without the callback, nothing to tell.
    lt_node_init(&n, &cfg, &OPS, 0);
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&n, t);
    nbp_name(obj, "Personal LaserWriter NTR");
    lt_node_rx(&n, f, lkup_from_83(f, obj, "LaserWriter"), t);
    CHECK_EQ_INT(nchecked, 1);
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
    test_pap_mac_reads();
    test_pap_superseded();
    test_pap_printer();
    test_pap_quiet();
    test_pap_fixture();
    test_elap_init_quiet();
    test_elap_lookup();
    test_elap_other_net();
    test_elap_rtmp();
    test_idle_hold();
    test_cable_no_idle_hold();
    test_idle_hold_waits_for_room();
    test_seen();
    test_nbp_utf8();
    test_register_two();
    test_open_on_reg();
    test_marker();
    test_pap_relayed_jobs();
    test_register_needs_room();
    test_register_macroman();
    test_name_asked();
    test_name_late_mac();
    test_name_after_registration();
    test_name_answered();
    test_name_other_replies();
    test_name_back_after_quiet();
    test_name_paced();
    test_names_off();
    test_printer_not_a_mac();
    test_serve_off();
    test_lookup_out();
    test_name_check();
    TEST_END;
}
