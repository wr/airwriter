// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PAP client (W-1082): the board printing to a LaserWriter, driven with
// a fake clock and a scripted printer at node 12 (listening socket 0x85,
// session socket 0x86). Every frame the node sends is captured.
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "pap_client.h"
#include <stdlib.h>

#define PR       12
#define PR_SLS   0x85
#define PR_SESS  0x86
#define ME       200

// ------------------------------------------------------------- the wire

#define MAXSENT 256
static uint8_t sent[MAXSENT][LLAP_MAX];
static size_t  sent_len[MAXSENT];
static int     nsent;
static size_t  biggest_data;          // the largest ATP payload the node sent

static void f_send(void *ctx, const uint8_t *f, size_t n)
{
    (void)ctx;
    memcpy(sent[nsent % MAXSENT], f, n);          // a ring: the last MAXSENT frames
    sent_len[nsent % MAXSENT] = n;
    nsent++;
    ddp_t d;
    atp_t a;
    if (!ddp_parse(f, n, &d) && d.type == DDP_ATP && !atp_parse(d.data, d.len, &a) &&
        a.len > biggest_data)
        biggest_data = a.len;
}
static void f_claim(void *ctx, uint8_t node) { (void)ctx; (void)node; }
static char lastlog[200];
static void f_log(void *ctx, const char *line)
{
    (void)ctx;
    strncpy(lastlog, line, sizeof lastlog - 1);
}
static const lt_ops_t NODE_OPS = { NULL, f_send, f_claim, f_log, NULL, NULL, NULL, NULL, NULL,
                                   NULL, NULL, NULL };

// Frame i as DDP and ATP; 0 when it is one.
static int frame(int i, ddp_t *d, atp_t *a)
{
    if (i < 0 || i >= nsent || i < nsent - MAXSENT) return -1;
    if (ddp_parse(sent[i % MAXSENT], sent_len[i % MAXSENT], d) || d->type != DDP_ATP) return -1;
    return atp_parse(d->data, d->len, a);
}

// The printer sends us an ATP packet from `ssock` to our `dsock`.
static void from_printer(lt_node_t *n, uint8_t ssock, uint8_t dsock, uint8_t ctl, uint8_t bitmap,
                         uint16_t tid, uint8_t u0, uint8_t u1, uint8_t u2, uint8_t u3,
                         const uint8_t *data, size_t len, uint64_t now)
{
    uint8_t a[ATP_HDR + ATP_MAX_DATA], f[LLAP_MAX];
    uint8_t user[4] = { u0, u1, u2, u3 };
    size_t al = atp_build(a, sizeof a, ctl, bitmap, tid, user, data, len);
    lt_addr_t to = { 0, ME, dsock }, from = { 0, PR, ssock };
    size_t fl = ddp_build(f, sizeof f, ME, PR, 0, &to, &from, DDP_ATP, a, al);
    lt_node_rx(n, f, fl, now);
}

// ------------------------------------------------------------- the job

static uint8_t  src[64 * 1024];
static size_t   src_len, src_pos;
static int      src_eof, reads;
static size_t f_read(void *ctx, uint8_t *buf, size_t max, int *eof)
{
    (void)ctx;
    reads++;
    size_t n = src_len - src_pos < max ? src_len - src_pos : max;
    memcpy(buf, src + src_pos, n);
    src_pos += n;
    *eof = src_eof && src_pos == src_len;
    return n;
}
static char  output[1024];
static size_t output_n;
static void f_output(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (output_n + n < sizeof output) { memcpy(output + output_n, d, n); output_n += n; }
}
static char status[64];
static int  nstatus;
static void f_status(void *ctx, const uint8_t *p)
{
    (void)ctx;
    memcpy(status, p + 1, p[0]);
    status[p[0]] = '\0';
    nstatus++;
}
static int opened, ended = -1, nended;
static void f_opened(void *ctx) { (void)ctx; opened++; }
static void f_ended(void *ctx, papc_end_t how) { (void)ctx; ended = (int)how; nended++; }
static const papc_ops_t OPS = { NULL, f_read, f_output, f_status, f_opened, f_ended, f_log };

static lt_node_t node;
static pap_client_t *pc;
static const lt_route_t SLS = { { 0, PR, PR_SLS }, PR, 0 };

static uint64_t fresh(void)
{
    lt_cfg_t cfg = { "ImageWriter II", "ImageWriter", 0, ME, 1, NULL, NULL };
    lt_node_init(&node, &cfg, &NODE_OPS, 0);
    uint64_t t = 0;
    for (int i = 0; i <= LT_ENQ_TRIES; i++, t += LT_ENQ_GAP_MS) lt_node_tick(&node, t);
    if (!pc) pc = malloc(sizeof *pc);
    papc_init(pc, &OPS, 7);
    lt_node_attach(&node, pc);
    nsent = 0;
    src_len = src_pos = 0; src_eof = 0; reads = 0;
    output_n = 0; nstatus = 0; opened = 0; ended = -1; nended = 0;
    return t;
}

// The OpenConn just sent: its TID and connection ID.
static void last_open(uint16_t *tid, uint8_t *cid, uint16_t *wait)
{
    ddp_t d; atp_t a;
    int i = nsent - 1;
    while (i >= 0 && i >= nsent - MAXSENT && (frame(i, &d, &a) || (a.ctl & ATP_FN_MASK) != ATP_TREQ ||
                      a.user[1] != PAP_OPEN_CONN))
        i--;
    CHECK(i >= 0);
    CHECK_EQ_INT(frame(i, &d, &a), 0);
    CHECK_EQ_INT(d.dst.node, PR);
    CHECK_EQ_INT(d.dst.sock, PR_SLS);
    CHECK_EQ_INT(d.src.sock, LT_SOCK_PAPC);
    CHECK_EQ_INT(a.ctl & (ATP_FN_MASK | ATP_XO), ATP_TREQ | ATP_XO);
    CHECK_EQ_INT(a.user[1], PAP_OPEN_CONN);
    CHECK_EQ_INT(a.len, 4);
    CHECK_EQ_INT(a.data[0], LT_SOCK_PAPC);
    CHECK_EQ_INT(a.data[1], 8);
    *tid = a.tid;
    *cid = a.user[0];
    *wait = (uint16_t)(a.data[2] << 8 | a.data[3]);
}

static void reply_open(uint16_t tid, uint8_t cid, uint16_t result, const char *st, uint64_t now)
{
    uint8_t data[4 + 1 + 64] = { PR_SESS, 8, (uint8_t)(result >> 8), (uint8_t)result };
    size_t k = strlen(st);
    data[4] = (uint8_t)k;
    memcpy(data + 5, st, k);
    from_printer(&node, PR_SLS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, tid, cid, PAP_OPEN_CONN_REPLY,
                 0, 0, data, 5 + k, now);
}

// Opens a connection; returns the connection ID.
static uint8_t open_ok(uint64_t t)
{
    CHECK_EQ_INT(papc_open(pc, &SLS, 600000, t), 0);
    uint16_t tid, wait;
    uint8_t cid;
    last_open(&tid, &cid, &wait);
    reply_open(tid, cid, 0, "status: idle", t + 5);
    CHECK_EQ_INT(opened, 1);
    return cid;
}

// The printer asks for data: SendData seq, its TID, 8 packets.
static int ask(uint8_t cid, uint16_t seq, uint16_t tid, uint64_t now)
{
    int before = nsent;
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TREQ | ATP_XO, 0xFF, tid, cid, PAP_SEND_DATA,
                 (uint8_t)(seq >> 8), (uint8_t)seq, NULL, 0, now);
    return before;
}

// Data packets sent since frame `from` in answer to TID `tid`: count, bytes, EOF.
static int answers(int from, uint16_t tid, size_t *bytes, int *eof, int *eom_last)
{
    int k = 0;
    *bytes = 0; *eof = -1; *eom_last = 0;
    for (int i = from; i < nsent; i++) {
        ddp_t d; atp_t a;
        if (frame(i, &d, &a) || (a.ctl & ATP_FN_MASK) != ATP_TRESP || a.tid != tid) continue;
        if (a.user[1] != PAP_DATA) continue;
        CHECK_EQ_INT(a.user[3], 0);                       // never the sequence number
        CHECK_EQ_INT(a.bitmap, k);                        // packet sequence 0, 1, 2 ...
        CHECK_EQ_INT(d.src.sock, LT_SOCK_PAPC);
        if (*eof < 0) *eof = a.user[2];
        else CHECK_EQ_INT(a.user[2], *eof);               // the same flag on every packet
        *bytes += a.len;
        *eom_last = !!(a.ctl & ATP_EOM);
        k++;
    }
    return k;
}

// Our last read of the printer (SendData to its session socket): its TID and seq.
static int last_read(int from, uint16_t *tid, uint16_t *seq)
{
    for (int i = nsent - 1; i >= from; i--) {
        ddp_t d; atp_t a;
        if (frame(i, &d, &a) || (a.ctl & ATP_FN_MASK) != ATP_TREQ || a.user[1] != PAP_SEND_DATA)
            continue;
        CHECK_EQ_INT(d.dst.sock, PR_SESS);
        *tid = a.tid;
        *seq = (uint16_t)(a.user[2] << 8 | a.user[3]);
        return 1;
    }
    return 0;
}

static int count_fn(int from, uint8_t fn, uint8_t ctl_fn)
{
    int k = 0;
    for (int i = from; i < nsent; i++) {
        ddp_t d; atp_t a;
        if (!frame(i, &d, &a) && (a.ctl & ATP_FN_MASK) == ctl_fn && a.user[1] == fn) k++;
    }
    return k;
}

// ------------------------------------------------------------- tests

static void test_busy(void)
{
    uint64_t t = fresh();
    CHECK_EQ_INT(papc_open(pc, &SLS, 600000, t), 0);
    CHECK_EQ_INT(papc_open(pc, &SLS, 600000, t), -1);    // one connection at a time
    uint16_t tid, wait;
    uint8_t cid;
    last_open(&tid, &cid, &wait);
    CHECK(cid != 0);
    CHECK_EQ_INT(wait, 0);
    reply_open(tid, cid, PAP_RESULT_BUSY, "busy; source: AppleTalk", t + 10);
    CHECK_EQ_INT(nstatus, 1);
    CHECK(strcmp(status, "busy; source: AppleTalk") == 0);
    int n0 = count_fn(0, PAP_OPEN_CONN, ATP_TREQ);
    lt_node_tick(&node, t + 1990);
    CHECK_EQ_INT(count_fn(0, PAP_OPEN_CONN, ATP_TREQ), n0);
    lt_node_tick(&node, t + 2000);
    CHECK_EQ_INT(count_fn(0, PAP_OPEN_CONN, ATP_TREQ), n0 + 1);
    uint16_t tid2;
    last_open(&tid2, &cid, &wait);
    CHECK(tid2 != tid);
    CHECK_EQ_INT(wait, 2);
    reply_open(tid2, cid, PAP_RESULT_BUSY, "busy; source: AppleTalk", t + 2010);
    lt_node_tick(&node, t + 4000);
    last_open(&tid, &cid, &wait);
    CHECK_EQ_INT(wait, 4);

    // Busy until the stall limit: given up.
    uint64_t now = t + 4000;
    while (nended == 0 && now < t + 700000) {
        last_open(&tid, &cid, &wait);
        reply_open(tid, cid, PAP_RESULT_BUSY, "busy; source: AppleTalk", now + 10);
        now += 2000;
        lt_node_tick(&node, now);
    }
    CHECK_EQ_INT(ended, PAPC_END_BUSY);
    CHECK(now >= t + 600000 && now <= t + 602000);
    CHECK(papc_idle(pc));

    // Canceled while waiting: ended, and no more OpenConn.
    t = fresh();
    papc_open(pc, &SLS, 600000, t);
    last_open(&tid, &cid, &wait);
    reply_open(tid, cid, PAP_RESULT_BUSY, "busy", t + 10);
    papc_cancel(pc, t + 500);
    CHECK_EQ_INT(ended, PAPC_END_CANCELED);
    n0 = count_fn(0, PAP_OPEN_CONN, ATP_TREQ);
    lt_node_tick(&node, t + 5000);
    CHECK_EQ_INT(count_fn(0, PAP_OPEN_CONN, ATP_TREQ), n0);

    // A printer that never answers: given up after 30 s, not 600.
    t = fresh();
    papc_open(pc, &SLS, 600000, t);
    for (uint64_t k = 0; k <= 31000 && nended == 0; k += 500) lt_node_tick(&node, t + k);
    CHECK_EQ_INT(ended, PAPC_END_DEAD);
    CHECK(count_fn(0, PAP_OPEN_CONN, ATP_TREQ) > 5);     // retried meanwhile
}

static void test_data(void)
{
    uint64_t t = fresh();
    uint8_t cid = open_ok(t);
    uint16_t rtid, rseq;
    CHECK(last_read(0, &rtid, &rseq));                    // we read the printer from the start
    CHECK_EQ_INT(rseq, 1);

    // 10 KB queued: SendData seq 1 gets 8 full packets.
    for (size_t i = 0; i < 10240; i++) src[i] = (uint8_t)(i * 7);
    src_len = 10240;
    t += 100;
    int from = ask(cid, 1, 0x500, t);
    lt_node_tick(&node, t);
    size_t bytes; int eof, eom;
    CHECK_EQ_INT(answers(from, 0x500, &bytes, &eof, &eom), 8);
    CHECK_EQ_INT(bytes, 4096);
    CHECK_EQ_INT(eof, 0);
    CHECK(eom);

    // The same SendData again (an ATP retransmit, or the same seq on a new
    // TID): the same packets, nothing new read.
    int r = reads;
    size_t pos = src_pos;
    from = ask(cid, 1, 0x500, t + 50);
    lt_node_tick(&node, t + 50);
    CHECK_EQ_INT(answers(from, 0x500, &bytes, &eof, &eom), 8);
    CHECK_EQ_INT(src_pos, pos);
    from = ask(cid, 1, 0x501, t + 60);
    lt_node_tick(&node, t + 60);
    CHECK_EQ_INT(answers(from, 0x501, &bytes, &eof, &eom), 8);
    CHECK_EQ_INT(bytes, 4096);
    CHECK_EQ_INT(src_pos, pos);
    (void)r;

    // 700 bytes and more to come: held up to 500 ms, then 512 + 188.
    src_len = src_pos + 700;
    from = ask(cid, 2, 0x502, t + 100);
    lt_node_tick(&node, t + 100);
    CHECK_EQ_INT(answers(from, 0x502, &bytes, &eof, &eom), 0);
    lt_node_tick(&node, t + 599);
    CHECK_EQ_INT(answers(from, 0x502, &bytes, &eof, &eom), 0);
    lt_node_tick(&node, t + 600);
    CHECK_EQ_INT(answers(from, 0x502, &bytes, &eof, &eom), 2);
    CHECK_EQ_INT(bytes, 700);
    CHECK_EQ_INT(eof, 0);

    // The printer's output, read back: to `output`, and the next read goes out.
    const char line[] = "%%[ status: busy; source: AppleTalk ]%%\r";
    from = nsent;
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, rtid, cid, PAP_DATA, 0, 0,
                 (const uint8_t *)line, sizeof line - 1, t + 700);
    CHECK_EQ_INT(output_n, sizeof line - 1);
    CHECK(memcmp(output, line, sizeof line - 1) == 0);
    CHECK_EQ_INT(count_fn(from, 0, ATP_TREL), 1);         // the read released (user bytes 0)
    CHECK(last_read(from, &rtid, &rseq));
    CHECK_EQ_INT(rseq, 2);

    // A tickle every 30 s while the connection is open.
    from = nsent;
    lt_node_tick(&node, t + 30000);
    CHECK(count_fn(from, PAP_TICKLE, ATP_TREQ) == 1);
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TREQ, 0, 0x900, cid, PAP_TICKLE, 0, 0, NULL, 0,
                 t + 30000);

    // The rest, then EOF: the last answer says so on every packet.
    src_len = src_pos + 1000;
    src_eof = 1;
    from = ask(cid, 3, 0x503, t + 31000);
    lt_node_tick(&node, t + 31000);
    CHECK_EQ_INT(answers(from, 0x503, &bytes, &eof, &eom), 2);
    CHECK_EQ_INT(bytes, 1000);
    CHECK_EQ_INT(eof, 1);
    // A SendData after our EOF: an empty EOF answer.
    from = ask(cid, 4, 0x504, t + 31100);
    lt_node_tick(&node, t + 31100);
    CHECK_EQ_INT(answers(from, 0x504, &bytes, &eof, &eom), 1);
    CHECK_EQ_INT(bytes, 0);
    CHECK_EQ_INT(eof, 1);
    CHECK_EQ_INT(nended, 0);

    // The printer's own EOF on our read: CloseConn to its session socket, then done.
    CHECK(last_read(0, &rtid, &rseq));
    from = nsent;
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, rtid, cid, PAP_DATA, 1, 0,
                 NULL, 0, t + 32000);
    CHECK_EQ_INT(count_fn(from, PAP_CLOSE_CONN, ATP_TREQ), 1);
    ddp_t d; atp_t a;
    uint16_t ctid = 0;
    for (int i = from; i < nsent; i++)
        if (!frame(i, &d, &a) && a.user[1] == PAP_CLOSE_CONN) { ctid = a.tid; CHECK_EQ_INT(d.dst.sock, PR_SESS); }
    CHECK_EQ_INT(nended, 0);
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, ctid, cid,
                 PAP_CLOSE_CONN_REPLY, 0, 0, NULL, 0, t + 32010);
    CHECK_EQ_INT(ended, PAPC_END_OK);
    CHECK(papc_idle(pc));
    CHECK(biggest_data <= PAP_QUANTUM);
}

static void test_seq_wrap(void)
{
    uint64_t t = fresh();
    uint8_t cid = open_ok(t);
    src_len = 8192;
    int from = ask(cid, 65535, 0x600, t + 10);
    lt_node_tick(&node, t + 10);
    size_t b1, b2; int eof, eom;
    CHECK_EQ_INT(answers(from, 0x600, &b1, &eof, &eom), 8);
    size_t pos = src_pos;
    from = ask(cid, 1, 0x601, t + 20);                     // 0 is skipped: 1 is new
    lt_node_tick(&node, t + 20);
    CHECK_EQ_INT(answers(from, 0x601, &b2, &eof, &eom), 8);
    CHECK_EQ_INT(src_pos, pos + 4096);
}

static void test_no_printer_eof(void)
{
    uint64_t t = fresh();
    uint8_t cid = open_ok(t);
    src_len = 10; src_eof = 1;
    int from = ask(cid, 1, 0x700, t + 10);
    lt_node_tick(&node, t + 10);
    size_t bytes; int eof, eom;
    CHECK_EQ_INT(answers(from, 0x700, &bytes, &eof, &eom), 1);
    CHECK_EQ_INT(eof, 1);
    // The printer keeps the connection (tickles) but never sends its EOF.
    from = nsent;
    for (uint64_t k = 1000; k < 29000; k += 1000) {
        from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TREQ, 0, 0x910, cid, PAP_TICKLE, 0, 0,
                     NULL, 0, t + 10 + k);
        lt_node_tick(&node, t + 10 + k);
    }
    CHECK_EQ_INT(count_fn(from, PAP_CLOSE_CONN, ATP_TREQ), 0);
    lt_node_tick(&node, t + 10 + 30000);
    CHECK_EQ_INT(count_fn(from, PAP_CLOSE_CONN, ATP_TREQ), 1);
    CHECK(strstr(lastlog, "no EOF") != NULL);
    // No CloseConnReply either: given up after its retries, still a printed job.
    for (uint64_t k = 30000; k < 40000 && !nended; k += 500) lt_node_tick(&node, t + 10 + k);
    CHECK_EQ_INT(ended, PAPC_END_OK);
}

static void test_dead_and_dropped(void)
{
    uint64_t t = fresh();
    uint8_t cid = open_ok(t);
    (void)cid;
    for (uint64_t k = 0; k < 119000; k += 1000) lt_node_tick(&node, t + k);   // from before open
    CHECK_EQ_INT(nended, 0);
    lt_node_tick(&node, t + 120010);
    CHECK_EQ_INT(ended, PAPC_END_DEAD);

    t = fresh();
    open_ok(t);
    papc_set_dead_ms(pc, 5000);
    lt_node_tick(&node, t + 5010);
    CHECK_EQ_INT(ended, PAPC_END_DEAD);

    t = fresh();
    open_ok(t);
    papc_link_down(pc, t + 100);
    CHECK_EQ_INT(ended, PAPC_END_DROPPED);
    CHECK(papc_idle(pc));

    // The printer closes before our EOF (a PostScript error flushed, then
    // closed): the job did not print.
    t = fresh();
    cid = open_ok(t);
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TREQ | ATP_XO, 0x01, 0x920, cid, PAP_CLOSE_CONN,
                 0, 0, NULL, 0, t + 100);
    CHECK_EQ_INT(count_fn(0, PAP_CLOSE_CONN_REPLY, ATP_TRESP), 1);
    CHECK_EQ_INT(ended, PAPC_END_DROPPED);
}

static void test_cancel_open(void)
{
    uint64_t t = fresh();
    uint8_t cid = open_ok(t);
    src_len = 20000;
    papc_cancel(pc, t + 10);
    int from = ask(cid, 1, 0x800, t + 20);
    lt_node_tick(&node, t + 20);
    size_t bytes; int eof, eom;
    CHECK_EQ_INT(answers(from, 0x800, &bytes, &eof, &eom), 1);
    CHECK_EQ_INT(bytes, 0);
    CHECK_EQ_INT(eof, 1);
    uint16_t rtid, rseq;
    CHECK(last_read(0, &rtid, &rseq));
    from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, rtid, cid, PAP_DATA, 1, 0,
                 NULL, 0, t + 100);
    for (int i = 0; i < nsent; i++) {
        ddp_t d; atp_t a;
        if (!frame(i, &d, &a) && a.user[1] == PAP_CLOSE_CONN)
            from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, a.tid, cid,
                         PAP_CLOSE_CONN_REPLY, 0, 0, NULL, 0, t + 110);
    }
    CHECK_EQ_INT(ended, PAPC_END_CANCELED);
}

// Canceled while the printer pulls nothing (out of paper): our EOF would wait
// for paper, and the job with it, holding the board (W-1082 bench). After
// PAPC_CANCEL_WAIT_MS with no SendData the client closes the connection,
// which the printer takes as the job's end and flushes.
static void test_cancel_paused(void)
{
    uint64_t t = fresh();
    uint8_t cid = open_ok(t);
    src_len = 20000;
    papc_cancel(pc, t + 10);
    int from = nsent;
    for (uint64_t k = 0; k < PAPC_CANCEL_WAIT_MS - 1000; k += 500) {
        if (k % 5000 == 0)                       // a paused printer still tickles
            from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TREQ, 0, 0x930, cid, PAP_TICKLE, 0, 0,
                         NULL, 0, t + 10 + k);
        lt_node_tick(&node, t + 10 + k);
    }
    CHECK_EQ_INT(count_fn(from, PAP_CLOSE_CONN, ATP_TREQ), 0);
    lt_node_tick(&node, t + 10 + PAPC_CANCEL_WAIT_MS);
    CHECK_EQ_INT(count_fn(from, PAP_CLOSE_CONN, ATP_TREQ), 1);
    for (int i = from; i < nsent; i++) {
        ddp_t d; atp_t a;
        if (!frame(i, &d, &a) && a.user[1] == PAP_CLOSE_CONN)
            from_printer(&node, PR_SESS, LT_SOCK_PAPC, ATP_TRESP | ATP_EOM, 0, a.tid, cid,
                         PAP_CLOSE_CONN_REPLY, 0, 0, NULL, 0, t + 20 + PAPC_CANCEL_WAIT_MS);
    }
    CHECK_EQ_INT(ended, PAPC_END_CANCELED);
}

static void test_status(void)
{
    uint64_t t = fresh();
    papc_status(pc, &SLS, t);
    ddp_t d; atp_t a;
    CHECK_EQ_INT(frame(nsent - 1, &d, &a), 0);
    CHECK_EQ_INT(d.src.sock, LT_SOCK_PAPC_ST);
    CHECK_EQ_INT(d.dst.sock, PR_SLS);
    CHECK_EQ_INT(a.user[0], 0);
    CHECK_EQ_INT(a.user[1], PAP_SEND_STATUS);
    uint8_t data[4 + 1 + 40] = { 0, 0, 0, 0 };
    const char *st = "status: PrinterError: out of paper";
    data[4] = (uint8_t)strlen(st);
    memcpy(data + 5, st, strlen(st));
    from_printer(&node, PR_SLS, LT_SOCK_PAPC_ST, ATP_TRESP | ATP_EOM, 0, a.tid, 0, PAP_STATUS, 0, 0,
                 data, 5 + strlen(st), t + 20);
    CHECK_EQ_INT(nstatus, 1);
    CHECK(strcmp(status, st) == 0);
    CHECK(papc_idle(pc));                                 // a status needs no connection

    // Unanswered, it is asked again, a few times.
    t += 100;
    int from = nsent;
    papc_status(pc, &SLS, t);
    for (uint64_t k = 0; k < 10000; k += 500) lt_node_tick(&node, t + k);
    int asks = count_fn(from, PAP_SEND_STATUS, ATP_TREQ);
    CHECK(asks >= 2 && asks <= 4);
}

int main(void)
{
    test_busy();
    test_data();
    test_seq_wrap();
    test_no_printer_eof();
    test_dead_and_dropped();
    test_cancel_open();
    test_cancel_paused();
    test_status();
    free(pc);
    TEST_END;
}
