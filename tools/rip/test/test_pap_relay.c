// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PAP relay (W-1088): a Mac on one LocalTalk wire prints to a printer on
// another through the board. Four nodes in one process, on a fake clock:
//
//   wire 1: the Mac (node 130, a PAP client as the Mac's driver is one) and the
//           board's Mac side (node 140, offering "Office":LaserWriter at 0x85)
//   wire 2: the board's printer side (node 150, its PAP client) and the
//           printer (node 160, "Office":LaserWriter at 0x85, a scripted
//           LaserWriter: it pulls the job and gives its own output back)
//
// The test plays the board's laser task: it acts on pap_relay_tick's asks.
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "pap_client.h"
#include "pap_relay.h"
#include <stdlib.h>

enum { MAC, BOARD_B, BOARD_A, PRN, NODES };
static const uint8_t HINT[NODES] = { 130, 140, 150, 160 };   // server IDs: 128-254
static lt_node_t node[NODES];
static int       wire_of[NODES] = { 1, 1, 2, 2 };

// ------------------------------------------------------------- the wires

#define QMAX 512
static uint8_t q[QMAX][LLAP_MAX];
static size_t  q_len[QMAX];
static int     q_from[QMAX];
static int     q_n;

static void w_send(void *ctx, const uint8_t *f, size_t n)
{
    int who = (int)(intptr_t)ctx;
    if (q_n >= QMAX) return;
    memcpy(q[q_n], f, n);
    q_len[q_n] = n;
    q_from[q_n] = who;
    q_n++;
}

static void deliver(uint64_t now)
{
    for (int k = 0; k < q_n; k++)
        for (int i = 0; i < NODES; i++)
            if (i != q_from[k] && wire_of[i] == wire_of[q_from[k]]) lt_node_rx(&node[i], q[k], q_len[k], now);
    q_n = 0;
}

static void w_claim(void *ctx, uint8_t n) { (void)ctx; (void)n; }
static void w_log(void *ctx, const char *line) { (void)ctx; (void)line; }

// ------------------------------------------------------------- the relay's two sides

static pap_relay_t relay;
static uint8_t     m2p[16384], p2m[16384];
static pap_client_t *bc, *mc;               // the board's client; the Mac

static int    b_pap_open(void *ctx, int reg, uint64_t now) { (void)ctx; return pap_relay_mac_open(&relay, &node[BOARD_B], reg, now); }
static void   b_data(void *ctx, const uint8_t *d, size_t n) { (void)ctx; pap_relay_mac_data(&relay, d, n); }
static size_t b_room(void *ctx) { (void)ctx; return pap_relay_mac_room(&relay); }
static void   b_end(void *ctx, int normal) { (void)ctx; pap_relay_mac_end(&relay, normal); }
static size_t b_status(void *ctx, int reg, uint8_t *p, size_t cap) { (void)ctx; (void)reg; return pap_relay_status(&relay, p, cap); }
static size_t b_output(void *ctx, int reg, uint8_t *buf, size_t max, int *eof, int again)
{
    (void)ctx; (void)reg;
    return pap_relay_mac_output(&relay, buf, max, eof, again);
}

static size_t bc_read(void *ctx, uint8_t *buf, size_t max, int *eof) { (void)ctx; return pap_relay_prn_read(&relay, buf, max, eof); }
static void   bc_output(void *ctx, const uint8_t *d, size_t n) { (void)ctx; pap_relay_prn_output(&relay, d, n); }
static void   bc_status(void *ctx, const uint8_t *p) { (void)ctx; pap_relay_prn_status(&relay, p); }
static void   bc_opened(void *ctx) { (void)ctx; pap_relay_prn_opened(&relay); }
static int    bc_end = -1;
static void   bc_ended(void *ctx, papc_end_t how) { (void)ctx; bc_end = (int)how; pap_relay_prn_ended(&relay, how); }

// ------------------------------------------------------------- the printer

static uint8_t p_job[64 * 1024];
static size_t  p_job_n;
static int     p_job_eof, p_busy_left, p_opens;
static uint8_t p_out[4096];
static size_t  p_out_len, p_out_pos;

static int p_pap_open(void *ctx, int reg, uint64_t now)
{
    (void)ctx; (void)reg; (void)now;
    p_opens++;
    if (p_busy_left > 0) { p_busy_left--; return 1; }
    p_job_n = 0; p_job_eof = 0; p_out_pos = 0;
    return 0;
}
static void   p_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (p_job_n + n <= sizeof p_job) { memcpy(p_job + p_job_n, d, n); p_job_n += n; }
}
static size_t p_room(void *ctx) { (void)ctx; return (size_t)-1; }
static void   p_end(void *ctx, int normal) { (void)ctx; if (normal) p_job_eof = 1; }
static size_t p_status(void *ctx, int reg, uint8_t *p, size_t cap)
{
    (void)ctx; (void)reg; (void)cap;
    static const char s[] = "status: idle";
    p[0] = (uint8_t)(sizeof s - 1);
    memcpy(p + 1, s, sizeof s - 1);
    return sizeof s;
}
// Its own output: some at once (a query's answers come as the job runs), the
// rest once the job's EOF is in; then its EOF.
static size_t p_output(void *ctx, int reg, uint8_t *buf, size_t max, int *eof, int again)
{
    (void)ctx; (void)reg; (void)again;
    size_t ready = p_job_eof ? p_out_len : p_out_len / 2;
    size_t n = ready - p_out_pos < max ? ready - p_out_pos : max;
    memcpy(buf, p_out + p_out_pos, n);
    p_out_pos += n;
    *eof = p_job_eof && p_out_pos == p_out_len;
    return n;
}

// ------------------------------------------------------------- the Mac

static uint8_t m_src[48 * 1024];
static size_t  m_src_len, m_src_pos;
static uint8_t m_out[4096];
static size_t  m_out_n;
static char    m_status[64];
static int     m_end = -1;

static size_t m_read(void *ctx, uint8_t *buf, size_t max, int *eof)
{
    (void)ctx;
    size_t n = m_src_len - m_src_pos < max ? m_src_len - m_src_pos : max;
    memcpy(buf, m_src + m_src_pos, n);
    m_src_pos += n;
    *eof = m_src_pos == m_src_len;
    return n;
}
static void m_output(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (m_out_n + n <= sizeof m_out) { memcpy(m_out + m_out_n, d, n); m_out_n += n; }
}
static void m_stat(void *ctx, const uint8_t *p)
{
    (void)ctx;
    memcpy(m_status, p + 1, p[0]);
    m_status[p[0]] = '\0';
}
static void m_ended(void *ctx, papc_end_t how) { (void)ctx; m_end = (int)how; }

// ------------------------------------------------------------- the run

static const lt_route_t B_REG = { { 0, 140, LT_SOCK_REG(1) }, 140, 0 };
static const lt_route_t P_REG = { { 0, 160, LT_SOCK_REG(1) }, 160, 0 };
static uint64_t T;
static int done_seen;

static void step(void)
{
    for (int i = 0; i < NODES; i++) lt_node_tick(&node[i], T);
    deliver(T);
    int act = pap_relay_tick(&relay, T);
    if (act & PR_RELAY_DO_OPEN) CHECK_EQ_INT(papc_open(bc, &P_REG, 60000, T), 0);
    if (act & PR_RELAY_DO_CANCEL) papc_cancel(bc, T);
    if (act & PR_RELAY_DO_ABORT_MAC) lt_node_pap_abort(&node[BOARD_B], T);
    if (act & PR_RELAY_DO_DONE) done_seen++;
    deliver(T);
    T += 10;
}

static void run_until(uint64_t until) { while (T < until) step(); }

static void setup(size_t job, size_t out)
{
    static const lt_ops_t base = { 0 };
    for (int i = 0; i < NODES; i++) {
        lt_ops_t ops = base;
        ops.ctx = (void *)(intptr_t)i;
        ops.send = w_send; ops.claim = w_claim; ops.log = w_log;
        if (i == BOARD_B) {
            ops.job_data = b_data; ops.job_room = b_room; ops.job_end = b_end;
            ops.pap_open = b_pap_open; ops.reg_status = b_status; ops.reg_output = b_output;
        } else if (i == PRN) {
            ops.job_data = p_data; ops.job_room = p_room; ops.job_end = p_end;
            ops.pap_open = p_pap_open; ops.reg_status = p_status; ops.reg_output = p_output;
        }
        lt_cfg_t cfg = { "", "ImageWriter", 0, HINT[i], (uint32_t)(i + 1), NULL };
        lt_node_init(&node[i], &cfg, &ops, 0);
    }
    q_n = 0;
    T = 0;
    run_until(2000);                              // every node has its ID
    for (int i = 0; i < NODES; i++) CHECK(node[i].phase == LT_ONLINE && node[i].node == HINT[i]);
    lt_offer_t off = { {0}, {0} };
    nbp_name(off.obj, "Office");
    nbp_name(off.type, "LaserWriter");
    lt_node_offer(&node[BOARD_B], NULL, &off, 1, NULL);
    lt_node_offer(&node[PRN], NULL, &off, 1, NULL);

    pap_relay_init(&relay, m2p, p2m, sizeof m2p);
    if (!bc) bc = malloc(sizeof *bc);
    if (!mc) mc = malloc(sizeof *mc);
    papc_ops_t bo = { NULL, bc_read, bc_output, bc_status, bc_opened, bc_ended, w_log };
    papc_ops_t mo = { NULL, m_read, m_output, m_stat, NULL, m_ended, w_log };
    papc_init(bc, &bo, 3);
    papc_init(mc, &mo, 5);
    lt_node_attach(&node[BOARD_A], bc);
    lt_node_attach(&node[MAC], mc);

    m_src_len = job; m_src_pos = 0;
    for (size_t i = 0; i < job; i++) m_src[i] = (uint8_t)(i * 7 + (i >> 9));
    p_out_len = out; p_out_pos = 0;
    for (size_t i = 0; i < out; i++) p_out[i] = (uint8_t)('A' + i % 26);
    p_job_n = 0; p_job_eof = 0; p_busy_left = 0; p_opens = 0;
    m_out_n = 0; m_end = -1; bc_end = -1; done_seen = 0; m_status[0] = '\0';
}

static void test_relay_bytes(void)
{
    setup(40 * 1024, 3 * 1024);
    CHECK_EQ_INT(papc_open(mc, &B_REG, 60000, T), 0);
    run_until(T + 120000);
    CHECK_EQ_INT(m_end, PAPC_END_OK);
    CHECK_EQ_INT(bc_end, PAPC_END_OK);
    CHECK_EQ_INT((int)p_job_n, 40 * 1024);
    CHECK(p_job_n == m_src_len && !memcmp(p_job, m_src, p_job_n));
    CHECK_EQ_INT((int)m_out_n, 3 * 1024);
    CHECK(!memcmp(m_out, p_out, m_out_n));
    CHECK(p_job_eof);
    CHECK_EQ_INT(done_seen, 1);
    CHECK(!pap_relay_active(&relay));
    CHECK(!node[BOARD_B].pap.open);
    CHECK(!node[PRN].pap.open);
}

static void test_relay_status(void)
{
    setup(1024, 0);
    papc_status(bc, &P_REG, T);                   // the board asks the printer, as laser.c does
    run_until(T + 1000);
    papc_status(mc, &B_REG, T);                   // the Mac asks the relayed name
    run_until(T + 1000);
    CHECK(!strcmp(m_status, "status: idle"));
}

static void test_relay_busy(void)
{
    setup(4096, 100);
    p_busy_left = 2;                               // the printer is busy twice
    CHECK_EQ_INT(papc_open(mc, &B_REG, 60000, T), 0);
    run_until(T + 3000);
    CHECK_EQ_INT(mc->phase, PAPC_OPENING);         // the Mac hears busy meanwhile
    run_until(T + 60000);
    CHECK(p_opens >= 3);
    CHECK_EQ_INT(m_end, PAPC_END_OK);
    CHECK_EQ_INT((int)p_job_n, 4096);
    CHECK_EQ_INT((int)m_out_n, 100);
    CHECK(!pap_relay_active(&relay));
}

static void test_relay_peer_closes(void)
{
    // The printer closes mid-job: the Mac's connection closes too.
    setup(40 * 1024, 0);
    CHECK_EQ_INT(papc_open(mc, &B_REG, 60000, T), 0);
    while (T < 60000 && p_job_n < 8192) step();
    CHECK(p_job_n >= 8192 && p_job_n < 40 * 1024);
    lt_node_pap_abort(&node[PRN], T);
    run_until(T + 10000);
    CHECK(bc_end == PAPC_END_DROPPED);
    CHECK(m_end == PAPC_END_DROPPED);
    CHECK(!node[BOARD_B].pap.open);
    CHECK(!pap_relay_active(&relay));

    // The Mac goes away mid-job: the printer's connection closes once the
    // board gives up on the Mac, and nothing is left in the relay.
    setup(40 * 1024, 0);
    CHECK_EQ_INT(papc_open(mc, &B_REG, 60000, T), 0);
    while (T < 60000 && p_job_n < 8192) step();
    papc_link_down(mc, T);
    lt_node_attach(&node[MAC], NULL);              // gone from the wire
    run_until(T + LT_PAP_CONN_TIMEOUT_MS + 40000);
    CHECK(!node[BOARD_B].pap.open);
    CHECK(!node[PRN].pap.open);
    CHECK(bc_end >= 0);
    CHECK(!pap_relay_active(&relay));
    CHECK_EQ_INT((int)pap_relay_mac_room(&relay), (int)sizeof m2p - 1);
}

int main(void)
{
    test_relay_bytes();
    test_relay_status();
    test_relay_busy();
    test_relay_peer_closes();
    TEST_END;
}
