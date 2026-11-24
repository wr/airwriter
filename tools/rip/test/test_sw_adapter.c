// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter adapter (W-1193), driven with a fake clock, a scripted Mac
// at node 20 and a fake printer behind the job calls. Every packet it sends
// is captured. The rename is the one a real Mac made to a real adapter
// (connection ID 000A, window 06B4); the print's made-up values are the
// protocol brief's: the Mac's control connection 4D01 and data connection
// 4D02, window 0800, data socket F1; ours A001 and, for the pipe, A002, whose
// window is the printer's room, 1000.
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "adsp.h"
#include "sw_adapter.h"
#include <stdlib.h>

#define US   SWA_SOCK(0)
#define US2  SWA_SOCK(1)
#define ME   77
static const lt_route_t MAC  = { { 0, 20, 0xE0 }, 20, 0 };   // its control socket
static const lt_route_t MACD = { { 0, 20, 0xF1 }, 20, 0 };   // its data socket
static const lt_route_t MACQ = { { 0, 20, 0xE1 }, 20, 0 };   // another control socket of its
static const lt_route_t MAC2 = { { 0, 21, 0xE0 }, 21, 0 };   // another Mac

static uint8_t REQ[SWA_REQ_LEN];        // socket F1, user "Bob"
static uint8_t REQ2[SWA_REQ_LEN];       // socket F2, user "Ann"
static uint8_t STATUS[SWA_STATUS_LEN];  // 0647 "Printing"

// ------------------------------------------------------------- the wire

#define MAXSENT 8192
typedef struct {
    lt_route_t to;
    uint8_t    sock;
    size_t     n;
    uint8_t    p[ADSP_HDR + ADSP_MAX_DATA];
} sent_t;
static sent_t       *sent;
static int           nsent;
static sw_adapter_t *A;
static uint64_t      T;

static void f_send(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n)
{
    (void)ctx;
    CHECK(nsent < MAXSENT && n <= sizeof sent[0].p);
    if (nsent >= MAXSENT || n > sizeof sent[0].p) return;
    sent[nsent].to = *to;
    sent[nsent].sock = src_sock;
    sent[nsent].n = n;
    memcpy(sent[nsent].p, p, n);
    nsent++;
}

// ------------------------------------------------------------- the board

#define MAXLOG 64
static char logs[MAXLOG][80];
static int  nlog;
static void f_log(void *ctx, const char *line)
{
    (void)ctx;
    // Never the Mac's user name, a printer's name or the status text.
    CHECK(!strstr(line, "Bob") && !strstr(line, "Ann") && !strstr(line, "Printing") &&
          !strstr(line, "StyleWriter") && !strstr(line, "Inkwell"));
    if (nlog < MAXLOG) snprintf(logs[nlog++], sizeof logs[0], "%s", line);
}
static int logged(const char *line)
{
    int k = 0;
    for (int i = 0; i < nlog; i++) if (!strcmp(logs[i], line)) k++;
    return k;
}

static int busy_val, begin_ret, n_begin, begin_id, n_up, n_end, end_clean, drained;
static int f_busy(void *ctx) { (void)ctx; return busy_val; }
static int f_begin(void *ctx, uint8_t id) { (void)ctx; n_begin++; begin_id = id; return begin_ret; }
static void f_up(void *ctx) { (void)ctx; n_up++; }
static void f_end(void *ctx, int clean) { (void)ctx; n_end++; end_clean = clean; }
static int f_drained(void *ctx) { (void)ctx; return drained; }

// The printer: a fixed room, or a buffer of sink_cap that held bytes fill.
static uint8_t printer[64 * 1024];
static size_t  printer_n, room_val, sink_cap, sink_held;
static size_t f_room(void *ctx) { (void)ctx; return sink_cap ? sink_cap - sink_held : room_val; }
static void f_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    CHECK(n > 0);
    CHECK(n <= f_room(NULL) + 1);               // + a held byte let go
    if (printer_n + n <= sizeof printer) { memcpy(printer + printer_n, d, n); printer_n += n; }
    if (sink_cap) sink_held += n;
}

static uint8_t out_q[4096];
static size_t  out_n, out_at;
static size_t f_output(void *ctx, uint8_t *buf, size_t max)
{
    (void)ctx;
    size_t n = out_n - out_at;
    if (n > max) n = max;
    memcpy(buf, out_q + out_at, n);
    out_at += n;
    return n;
}
static void printer_says(const void *d, size_t n)
{
    memcpy(out_q + out_n, d, n);
    out_n += n;
}

static int     n_rename, rename_id;
static uint8_t rename_name[NBP_NAME_MAX + 1];
static void f_rename(void *ctx, uint8_t id, const uint8_t *pname)
{
    (void)ctx;
    n_rename++; rename_id = id;
    memcpy(rename_name, pname, sizeof rename_name);
}

static const swa_ops_t OPS = { NULL, f_send, f_log, f_busy, f_begin, f_up, f_room, f_data,
                               f_output, f_drained, f_end, f_rename };

static swa_name_t NAMES[SWA_NAMES];

static void fresh(uint32_t seed)
{
    if (!A) A = malloc(sizeof *A);
    if (!sent) sent = malloc(MAXSENT * sizeof *sent);
    memset(REQ, 0, sizeof REQ);
    memcpy(REQ, "\x00\x00\x00\xF1\x03" "Bob", 8);
    memset(REQ2, 0, sizeof REQ2);
    memcpy(REQ2, "\x00\x00\x00\xF2\x03" "Ann", 8);
    memset(STATUS, 0, sizeof STATUS);
    memcpy(STATUS, "\x06\x47\x08" "Printing", 11);
    memset(NAMES, 0, sizeof NAMES);
    nbp_name(NAMES[0].obj, "Inkwell");
    nbp_name(NAMES[0].type, SWA_TYPE_COLOR);
    NAMES[0].id = 3;
    nsent = 0; T = 1000; nlog = 0;
    busy_val = 0; begin_ret = 0; n_begin = 0; begin_id = 0; n_up = 0; n_end = 0; end_clean = -1;
    drained = 1;
    printer_n = 0; room_val = 0x1000; sink_cap = 0; sink_held = 0;
    out_n = out_at = 0;
    n_rename = 0; rename_id = 0; memset(rename_name, 0, sizeof rename_name);
    swa_init(A, &OPS, seed);
    swa_offer(A, NAMES, 1, T);
}

static size_t pk(uint8_t *o, uint16_t id, uint32_t first, uint32_t next, uint16_t win,
                 uint8_t desc, const void *d, size_t n)
{
    adsp_hdr_t h = { id, first, next, win, desc };
    adsp_hdr_build(o, &h);
    if (n) memcpy(o + ADSP_HDR, d, n);
    return ADSP_HDR + n;
}

// A datagram of the given DDP type reaches our socket.
static void wire(const lt_route_t *from, uint8_t sock, uint8_t type, const uint8_t *p, size_t n)
{
    ddp_t d;
    memset(&d, 0, sizeof d);
    d.type = type;
    d.is_long = from->is_long;
    d.llap_src = from->via;
    d.llap_dst = ME;
    d.src = from->addr;
    d.dst.node = ME;
    d.dst.sock = sock;
    d.data = p;
    d.len = n;
    swa_rx(A, &d, T);
}

// The Mac sends us an ADSP packet, to socket sock, at T.
static void rx(const lt_route_t *from, uint8_t sock, uint16_t id, uint32_t first, uint32_t next,
               uint16_t win, uint8_t desc, const void *d, size_t n)
{
    uint8_t p[ADSP_HDR + 700];
    wire(from, sock, DDP_ADSP, p, pk(p, id, first, next, win, desc, d, n));
}

static void rx_open(const lt_route_t *from, uint8_t sock, uint16_t id, uint16_t win, uint8_t desc,
                    uint16_t dest)
{
    uint8_t par[8] = { 0x01, 0x00, (uint8_t)(dest >> 8), (uint8_t)dest, 0, 0, 0, 0 };
    rx(from, sock, id, 0, 0, win, desc, par, sizeof par);
}

// An attention message: its own sequence, no window.
static void rx_attn(const lt_route_t *from, uint8_t sock, uint16_t id, uint32_t seq, uint8_t desc,
                    uint16_t code, const void *body, size_t n)
{
    uint8_t d[2 + 600] = { (uint8_t)(code >> 8), (uint8_t)code };
    if (n) memcpy(d + 2, body, n);
    rx(from, sock, id, seq, 0, 0, desc, d, n + 2);
}

// Packet i we sent is exactly this, to that route, from socket sock_.
#define EXPECT(i, to_, sock_, id, first, next, win, desc, d_, len_) do { \
    uint8_t _w[ADSP_HDR + 700]; \
    size_t _wn = pk(_w, id, first, next, win, desc, d_, len_); \
    CHECK((i) < nsent); \
    if ((i) < nsent) { \
        CHECK_EQ_INT(sent[i].n, _wn); \
        if (sent[i].n == _wn) CHECK_EQ_MEM(sent[i].p, _w, _wn); \
        CHECK_EQ_INT(sent[i].to.addr.node, (to_)->addr.node); \
        CHECK_EQ_INT(sent[i].to.addr.sock, (to_)->addr.sock); \
        CHECK_EQ_INT(sent[i].to.via, (to_)->via); \
        CHECK_EQ_INT(sent[i].sock, sock_); \
    } } while (0)

#define EXPECT_OPEN(i, to_, sock_, id, win, desc, dest) do { \
    uint8_t _par[8] = { 0x01, 0x00, (uint8_t)((dest) >> 8), (uint8_t)(dest), 0, 0, 0, 0 }; \
    EXPECT(i, to_, sock_, id, 0, 0, win, desc, _par, 8); } while (0)

static uint8_t desc_of(int i) { return sent[i].p[12]; }
static int to_route(int i, const lt_route_t *r)
{
    return sent[i].to.addr.node == r->addr.node && sent[i].to.addr.sock == r->addr.sock;
}
// Packets with this descriptor sent to r, from index `from` on.
static int count_to(const lt_route_t *r, uint8_t desc, int from)
{
    int k = 0;
    for (int i = from; i < nsent; i++) if (to_route(i, r) && desc_of(i) == desc) k++;
    return k;
}

static void run(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 100) { T += 100; swa_tick(A, T); }
}

// ------------------------------------------------------------- a software Mac's connection

typedef struct {
    lt_route_t at;
    uint8_t    sock;                  // ours
    uint16_t   id, far, win;
    uint32_t   tx, rx, attn;
    int        mark, probed, closed;
    uint8_t    in[2048];              // stream bytes read since the last question
    size_t     in_n;
} mac_t;

// Read what the adapter has sent this connection.
static void m_poll(mac_t *m)
{
    for (; m->mark < nsent; m->mark++) {
        const sent_t *s = &sent[m->mark];
        adsp_hdr_t h;
        if (!to_route(m->mark, &m->at) || s->sock != m->sock) continue;
        if (adsp_hdr_parse(s->p, s->n, &h) || h.id != m->far) continue;
        if (h.desc == 0xC0) m->probed++;
        if (h.desc == 0x85) m->closed = 1;
        if (h.desc & 0x90) continue;
        size_t n = s->n - ADSP_HDR;
        if (h.first != m->rx || m->in_n + n > sizeof m->in) continue;
        memcpy(m->in + m->in_n, s->p + ADSP_HDR, n);
        m->in_n += n;
        m->rx += (uint32_t)n;
    }
}

static void m_ack(mac_t *m)
{
    rx(&m->at, m->sock, m->id, m->tx, m->rx, m->win, 0x80, NULL, 0);
}

// The Mac opens a connection to our socket: 1 when we answered.
static int m_open(mac_t *m, const lt_route_t *at, uint8_t sock, uint16_t id)
{
    memset(m, 0, sizeof *m);
    m->at = *at; m->sock = sock; m->id = id; m->win = 0x0800;
    int before = nsent;
    rx_open(at, sock, id, m->win, 0x81, 0);
    m->mark = nsent;
    if (nsent != before + 1 || desc_of(before) != 0x83 || !to_route(before, at)) return 0;
    m->far = (uint16_t)(sent[before].p[0] << 8 | sent[before].p[1]);
    rx_open(at, sock, id, m->win, 0x82, m->far);
    return 1;
}

// The Mac takes our call on `at`: the first Open Request there from index
// `from` on is answered. 1 when our third leg came back.
static int m_called(mac_t *m, const lt_route_t *at, uint8_t sock, uint16_t id, int from)
{
    memset(m, 0, sizeof *m);
    m->at = *at; m->sock = sock; m->id = id; m->win = 0x0800;
    for (int i = from; i < nsent; i++) {
        if (!to_route(i, at) || desc_of(i) != 0x81 || sent[i].sock != sock) continue;
        m->far = (uint16_t)(sent[i].p[0] << 8 | sent[i].p[1]);
        int before = nsent;
        rx_open(at, sock, id, m->win, 0x83, m->far);
        m->mark = nsent;
        return nsent == before + 1 && desc_of(before) == 0x82;
    }
    return 0;
}

// An attention message and the bytes that answer it (in m->in, m->in_n).
static const uint8_t *m_ask(mac_t *m, uint16_t code, const void *body, size_t n)
{
    m_poll(m);
    m->in_n = 0;
    rx_attn(&m->at, m->sock, m->id, m->attn++, 0x50, code, body, n);
    m_poll(m);
    m_ack(m);
    return m->in;
}

// Ask, and hold the 2-byte answer to b0 b1.
#define ASK2(m, code, body, n, b0, b1) do { \
    const uint8_t *_ans = m_ask(m, code, body, n); \
    CHECK_EQ_INT((m)->in_n, 2); \
    CHECK_EQ_INT(_ans[0], b0); CHECK_EQ_INT(_ans[1], b1); } while (0)

static void m_send(mac_t *m, uint8_t desc, const void *d, size_t n)
{
    rx(&m->at, m->sock, m->id, m->tx, m->rx, m->win, desc, d, n);
    m->tx += (uint32_t)n + ((desc & 0x20) ? 1 : 0);
}

static void m_close(mac_t *m)
{
    rx(&m->at, m->sock, m->id, m->tx, m->rx, 0, 0x85, NULL, 0);
}

// Time passes with these Macs alive: each answers our probes.
static void run_with(unsigned ms, mac_t *m1, mac_t *m2)
{
    mac_t *ms_[2] = { m1, m2 };
    for (unsigned t = 0; t < ms; t += 100) {
        T += 100;
        swa_tick(A, T);
        for (int i = 0; i < 2; i++) {
            if (!ms_[i]) continue;
            m_poll(ms_[i]);
            if (ms_[i]->probed) { ms_[i]->probed = 0; m_ack(ms_[i]); }
        }
    }
}

// A Mac asks to print and is accepted; nothing else yet.
static void ask_to_print(mac_t *ctl)
{
    fresh(0xA000);
    CHECK(m_open(ctl, &MAC, US, 0x4D01));
    ASK2(ctl, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(swa_job(A), 1);
    CHECK_EQ_INT(n_begin, 1);
}

// The whole handshake in Apple's order: the pipe is up.
static void start_job(mac_t *ctl, mac_t *pipe)
{
    ask_to_print(ctl);
    ASK2(ctl, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(ctl);
    CHECK(m_called(pipe, &MACD, US, 0x4D02, 0));
    CHECK_EQ_INT(swa_job(A), 2);
    CHECK_EQ_INT(n_up, 1);
}

// ------------------------------------------------------------- D.2 the real rename

static void test_rename_as_a_real_mac_did_it(void)
{
    static const uint8_t NAME[24] = "\x16" "Color StyleWriter 2200";
    fresh(9);                                   // the real adapter's connection ID was 000A
    rx_open(&MAC, US, 0x1234, 0x1000, 0x81, 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT_OPEN(0, &MAC, US, 0x000A, 0x06B4, 0x83, 0x1234);
    rx_open(&MAC, US, 0x1234, 0x1000, 0x82, 0x000A);
    CHECK_EQ_INT(nsent, 1);

    rx_attn(&MAC, US, 0x1234, 0, 0x50, 0x0011, "\0", 1);
    CHECK_EQ_INT(nsent, 3);
    EXPECT(1, &MAC, US, 0x000A, 0, 1, 0x0000, 0x90, NULL, 0);          // 13 bytes
    EXPECT(2, &MAC, US, 0x000A, 0, 0, 0x06B4, 0x40, "\0\0", 2);        // the answer, no EOM
    rx(&MAC, US, 0x1234, 0, 2, 0x1000, 0x80, NULL, 0);

    rx_attn(&MAC, US, 0x1234, 1, 0x50, 0x0009, NAME, sizeof NAME - 1);
    CHECK_EQ_INT(nsent, 5);
    EXPECT(3, &MAC, US, 0x000A, 0, 2, 0x0000, 0x90, NULL, 0);
    EXPECT(4, &MAC, US, 0x000A, 2, 0, 0x06B4, 0x40, "\0\0", 2);        // stream seq 2
    CHECK_EQ_INT(n_rename, 0);                  // staged, not applied
    rx(&MAC, US, 0x1234, 0, 4, 0x1000, 0x80, NULL, 0);

    rx_attn(&MAC, US, 0x1234, 2, 0x50, 0x0012, "\0", 1);
    CHECK_EQ_INT(nsent, 7);
    EXPECT(5, &MAC, US, 0x000A, 0, 3, 0x0000, 0x90, NULL, 0);
    EXPECT(6, &MAC, US, 0x000A, 4, 0, 0x06B4, 0x40, "\0\0", 2);        // stream seq 4
    CHECK_EQ_INT(n_rename, 1);
    CHECK_EQ_INT(rename_id, 3);
    CHECK_EQ_MEM(rename_name, NAME, sizeof NAME - 1);
    rx(&MAC, US, 0x1234, 0, 6, 0x1000, 0x80, NULL, 0);

    rx(&MAC, US, 0x1234, 0, 6, 0, 0x85, NULL, 0);
    CHECK_EQ_INT(nsent, 7);
    CHECK_EQ_INT(n_rename, 1);
    CHECK_EQ_INT(logged("adapter: rename asked"), 1);
    CHECK_EQ_INT(nlog, 1);
    CHECK_EQ_INT(n_begin, 0);
    CHECK_EQ_INT(swa_job(A), 0);
}

static void test_rename_needs_its_own_session_end(void)
{
    static const uint8_t NAME[6] = "\x04" "Quil";
    mac_t a, b;
    // 0009 and then a close with no 0012: unchanged.
    fresh(0xA000);
    CHECK(m_open(&a, &MAC, US, 0x4D01));
    ASK2(&a, SWA_SET_NAME, NAME, sizeof NAME - 1, 0x00, 0x00);
    m_close(&a);
    CHECK_EQ_INT(n_rename, 0);
    // 0012 on a different connection: still unchanged. On its own: renamed.
    CHECK(m_open(&a, &MAC, US, 0x4D01));
    CHECK(m_open(&b, &MAC2, US, 0x4D01));
    ASK2(&a, SWA_SET_NAME, NAME, sizeof NAME - 1, 0x00, 0x00);
    ASK2(&b, SWA_END, "\0", 1, 0x00, 0x00);
    CHECK_EQ_INT(n_rename, 0);
    ASK2(&a, SWA_END, "\0", 1, 0x00, 0x00);
    CHECK_EQ_INT(n_rename, 1);
    CHECK_EQ_MEM(rename_name, NAME, sizeof NAME - 1);
    ASK2(&a, SWA_END, "\0", 1, 0x00, 0x00);     // once
    CHECK_EQ_INT(n_rename, 1);
    // A connection that takes a closed one's place inherits nothing.
    ASK2(&b, SWA_SET_NAME, NAME, sizeof NAME - 1, 0x00, 0x00);
    m_close(&b);
    CHECK(m_open(&b, &MAC2, US, 0x4D07));
    ASK2(&b, SWA_END, "\0", 1, 0x00, 0x00);
    CHECK_EQ_INT(n_rename, 1);

    // Names that cannot be: every one answered 00 00, none applied.
    static const struct { const char *b; size_t n; } BAD[] = {
        { "\x00", 1 }, { "", 0 }, { "\x05" "Quil", 5 }, { "\x04" "Qu:l", 5 }, { "\x04" "Q@il", 5 },
        { "\x21" "123456789012345678901234567890123", 34 },
    };
    for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
        ASK2(&a, SWA_SET_NAME, BAD[i].b, BAD[i].n, 0x00, 0x00);
        ASK2(&a, SWA_END, "\0", 1, 0x00, 0x00);
        CHECK_EQ_INT(n_rename, 1);
    }
    // A bad one after a good one unstages it.
    ASK2(&a, SWA_SET_NAME, NAME, sizeof NAME - 1, 0x00, 0x00);
    ASK2(&a, SWA_SET_NAME, "\x00", 1, 0x00, 0x00);
    ASK2(&a, SWA_END, "\0", 1, 0x00, 0x00);
    CHECK_EQ_INT(n_rename, 1);
    // 32 characters, the longest, with a MacRoman byte.
    static const uint8_t LONG[34] = "\x20" "1234567890123456789012345678901\x8E";
    ASK2(&a, SWA_SET_NAME, LONG, sizeof LONG - 1, 0x00, 0x00);
    ASK2(&a, SWA_END, "\0", 1, 0x00, 0x00);
    CHECK_EQ_INT(n_rename, 2);
    CHECK_EQ_MEM(rename_name, LONG, sizeof LONG - 1);

    // The second name's socket renames the second record.
    fresh(0xA000);
    nbp_name(NAMES[1].obj, "Inkwell 2");
    nbp_name(NAMES[1].type, SWA_TYPE_MONO);
    NAMES[1].id = 5;
    swa_offer(A, NAMES, 2, T);
    CHECK(m_open(&a, &MAC, US2, 0x4D01));
    ASK2(&a, SWA_SET_NAME, NAME, sizeof NAME - 1, 0x00, 0x00);
    ASK2(&a, SWA_END, "\0", 1, 0x00, 0x00);
    CHECK_EQ_INT(n_rename, 1);
    CHECK_EQ_INT(rename_id, 5);

    // On the pipe 0012 ends the job and renames nothing.
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    ASK2(&pipe, SWA_SET_NAME, NAME, sizeof NAME - 1, 0x00, 0x00);
    ASK2(&pipe, SWA_END, "\0", 1, 0xFF, 0xFF);
    CHECK_EQ_INT(n_rename, 0);
}

// ------------------------------------------------------------- D.8 the whole print

static void test_whole_print_in_apples_order(void)
{
    fresh(0xA000);
    rx_open(&MAC, US, 0x4D01, 0x0800, 0x81, 0);                         //  1
    CHECK_EQ_INT(nsent, 1);
    EXPECT_OPEN(0, &MAC, US, 0xA001, 0x06B4, 0x83, 0x4D01);             //  2
    rx_open(&MAC, US, 0x4D01, 0x0800, 0x82, 0xA001);                    //  3
    rx_attn(&MAC, US, 0x4D01, 0, 0x50, 0x000B, REQ, sizeof REQ);        //  4
    CHECK_EQ_INT(nsent, 3);
    EXPECT(1, &MAC, US, 0xA001, 0, 1, 0x0000, 0x90, NULL, 0);           //  5
    EXPECT(2, &MAC, US, 0xA001, 0, 0, 0x06B4, 0x40, "\0\0", 2);         //  6 accepted
    CHECK_EQ_INT(n_begin, 1);
    CHECK_EQ_INT(begin_id, 3);
    CHECK_EQ_INT(swa_job(A), 1);
    CHECK_EQ_INT(logged("adapter: print request from 0.20"), 1);
    rx(&MAC, US, 0x4D01, 0, 2, 0x0800, 0x80, NULL, 0);                  //  7
    CHECK_EQ_INT(nsent, 3);                                             // no call yet

    rx_attn(&MAC, US, 0x4D01, 1, 0x50, 0x0012, "\0", 1);                //  8
    CHECK_EQ_INT(nsent, 6);
    EXPECT(3, &MAC, US, 0xA001, 0, 2, 0x0000, 0x90, NULL, 0);           //  9
    EXPECT(4, &MAC, US, 0xA001, 2, 0, 0x06B4, 0x40, "\0\0", 2);         // 10 answered first
    EXPECT_OPEN(5, &MACD, US, 0xA002, 0x1000, 0x81, 0);                 // 13 to socket F1
    rx(&MAC, US, 0x4D01, 0, 4, 0x0800, 0x80, NULL, 0);                  // 11
    rx(&MAC, US, 0x4D01, 0, 4, 0, 0x85, NULL, 0);                       // 12 control closed
    CHECK_EQ_INT(nsent, 6);                                             // one call, not two
    CHECK_EQ_INT(n_up, 0);

    rx_open(&MACD, US, 0x4D02, 0x0800, 0x83, 0xA002);                   // 14
    CHECK_EQ_INT(nsent, 7);
    EXPECT_OPEN(6, &MACD, US, 0xA002, 0x1000, 0x82, 0x4D02);            // 15
    CHECK_EQ_INT(n_up, 1);
    CHECK_EQ_INT(swa_job(A), 2);
    CHECK_EQ_INT(logged("adapter: pipe up"), 1);

    rx_attn(&MACD, US, 0x4D02, 0, 0x50, 0x000A, STATUS, sizeof STATUS); // 16
    CHECK_EQ_INT(nsent, 9);
    EXPECT(7, &MACD, US, 0xA002, 0, 1, 0x0000, 0x90, NULL, 0);          // 17
    EXPECT(8, &MACD, US, 0xA002, 0, 0, 0x1000, 0x40, "\0\0", 2);        // 18
    rx(&MACD, US, 0x4D02, 0, 2, 0x0800, 0x60, "\x00", 1);               // 19 M 0 -> 2
    CHECK_EQ_INT(nsent, 10);
    EXPECT(9, &MACD, US, 0xA002, 2, 2, 0x1000, 0x80, NULL, 0);
    rx(&MACD, US, 0x4D02, 2, 2, 0x0800, 0x60, "\xFF\xFF\xFF\x49", 4);   // 20 reset: M 2 -> 7
    CHECK_EQ_INT(nsent, 11);
    EXPECT(10, &MACD, US, 0xA002, 2, 7, 0x1000, 0x80, NULL, 0);         // 21
    run(2000);                                                          // (2 s pause)
    CHECK_EQ_INT(nsent, 11);

    rx(&MACD, US, 0x4D02, 7, 2, 0x0800, 0x60, "\xFF\xFF\xFF\x31", 4);   // 22 M 7 -> 12
    CHECK_EQ_INT(nsent, 12);
    EXPECT(11, &MACD, US, 0xA002, 2, 12, 0x1000, 0x80, NULL, 0);
    printer_says("\x08", 1);
    swa_tick(A, T);
    CHECK_EQ_INT(nsent, 13);
    EXPECT(12, &MACD, US, 0xA002, 2, 12, 0x1000, 0x40, "\x08", 1);      // 23 A 2 -> 3, no EOM
    rx(&MACD, US, 0x4D02, 12, 3, 0x0800, 0x60, "\x3F", 1);              // 24 M 12 -> 14
    CHECK_EQ_INT(nsent, 14);
    EXPECT(13, &MACD, US, 0xA002, 3, 14, 0x1000, 0x80, NULL, 0);
    printer_says("CS\r", 3);
    swa_tick(A, T);
    CHECK_EQ_INT(nsent, 15);
    EXPECT(14, &MACD, US, 0xA002, 3, 14, 0x1000, 0x40, "CS\r", 3);      // 25 A 3 -> 6

    drained = 0;                                                        // bytes still wait
    rx_attn(&MACD, US, 0x4D02, 1, 0x50, 0x0006, "\0", 1);               // 26 drained?
    CHECK_EQ_INT(nsent, 17);
    EXPECT(15, &MACD, US, 0xA002, 0, 2, 0x0000, 0x90, NULL, 0);         // 27
    EXPECT(16, &MACD, US, 0xA002, 6, 14, 0x1000, 0x40, "\0\0", 2);      // 28, not yet
    drained = 1;
    rx_attn(&MACD, US, 0x4D02, 2, 0x50, 0x0006, "\0", 1);
    CHECK_EQ_INT(nsent, 19);
    EXPECT(17, &MACD, US, 0xA002, 0, 3, 0x0000, 0x90, NULL, 0);
    EXPECT(18, &MACD, US, 0xA002, 8, 14, 0x1000, 0x40, "\xFF\xFF", 2);  // 28

    rx_attn(&MACD, US, 0x4D02, 3, 0x50, 0x0012, "\0", 1);               // 29 end session
    CHECK_EQ_INT(nsent, 21);
    EXPECT(19, &MACD, US, 0xA002, 0, 4, 0x0000, 0x90, NULL, 0);         // 30
    EXPECT(20, &MACD, US, 0xA002, 10, 14, 0x1000, 0x40, "\xFF\xFF", 2); // 31
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(swa_job(A), 2);
    rx(&MACD, US, 0x4D02, 14, 12, 0, 0x85, NULL, 0);                    // 32
    CHECK_EQ_INT(nsent, 21);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 1);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(logged("adapter: job ended cleanly"), 1);
    CHECK_EQ_INT(nlog, 3);

    // The printer: what the Mac sent, in order, and no reset of ours.
    CHECK_EQ_INT(printer_n, 10);
    CHECK_EQ_MEM(printer, "\x00\xFF\xFF\xFF\x49\xFF\xFF\xFF\x31\x3F", 10);
    CHECK_EQ_INT(n_begin, 1);
    CHECK_EQ_INT(n_up, 1);
    run(30000);
    CHECK_EQ_INT(nsent, 21);
    CHECK_EQ_INT(n_end, 1);
}

static void test_lpstyls_order(void)
{
    mac_t ctl, pipe;
    ask_to_print(&ctl);                         // and then nothing: no 0012, no close
    int mark = nsent;
    run(900);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 0);
    run(100);                                   // the call-back anyway, a second on
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, mark));
    CHECK_EQ_INT(swa_job(A), 2);

    // Its attentions ask for no ack; they get one all the same.
    m_poll(&pipe);
    pipe.in_n = 0;
    mark = nsent;
    rx_attn(&MACD, US, 0x4D02, pipe.attn++, 0x10, SWA_SET_STATUS, STATUS, sizeof STATUS);
    CHECK_EQ_INT(count_to(&MACD, 0x90, mark), 1);
    m_poll(&pipe);
    CHECK_EQ_INT(pipe.in_n, 2);
    CHECK_EQ_MEM(pipe.in, "\0\0", 2);
    m_ack(&pipe);

    m_send(&pipe, 0x60, "\x00", 1);
    m_send(&pipe, 0x60, "\xFF\xFF\xFF\x49", 4);
    m_send(&pipe, 0x60, "page", 4);
    // The end: a Forward Reset first, which we answer.
    mark = nsent;
    rx(&MACD, US, 0x4D02, pipe.tx, pipe.rx, 0x0800, 0x86, NULL, 0);
    CHECK_EQ_INT(count_to(&MACD, 0x87, mark), 1);
    m_send(&pipe, 0x60, "\x00", 1);
    m_send(&pipe, 0x60, "\xFF\xFF\xFF\x49", 4);
    m_poll(&pipe);
    pipe.in_n = 0;
    rx_attn(&MACD, US, 0x4D02, pipe.attn++, 0x10, SWA_END, "\0", 1);
    m_poll(&pipe);
    CHECK_EQ_INT(pipe.in_n, 2);
    CHECK_EQ_MEM(pipe.in, "\xFF\xFF", 2);
    m_close(&pipe);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 1);
    CHECK_EQ_INT(printer_n, 14);
    CHECK_EQ_MEM(printer, "\x00\xFF\xFF\xFF\x49page\x00\xFF\xFF\xFF\x49", 14);

    // The control connection it never closed is dropped once it stops
    // answering, and nothing comes of it.
    CHECK(adsp_is_open(&A->adsp, 0));
    run(25000);
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 1);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(n_begin, 1);
    CHECK_EQ_INT(swa_job(A), 0);
}

static void test_call_back_triggers(void)
{
    mac_t ctl, pipe;
    // The Mac's close alone, with no 0012.
    ask_to_print(&ctl);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 0);
    m_close(&ctl);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    run(900);                                   // and the second passing makes no other
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, 0));
    CHECK_EQ_INT(swa_job(A), 2);

    // 0012 on another connection is not the sign.
    mac_t other;
    ask_to_print(&ctl);
    CHECK(m_open(&other, &MAC2, US, 0x4D09));
    ASK2(&other, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(&other);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 0);

    // A control connection closing once the pipe is up calls nobody.
    ask_to_print(&ctl);
    run(1000);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, 0));
    m_close(&ctl);
    run_with(3000, &pipe, NULL);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    CHECK_EQ_INT(swa_job(A), 2);
    CHECK_EQ_INT(n_end, 0);

    // The call goes to the Mac's own address, long header and router and all.
    static const lt_route_t FAR = { { 0x1234, 9, 0xE0 }, 44, 1 };
    fresh(0xA000);
    CHECK(m_open(&ctl, &FAR, US, 0x4D01));
    ASK2(&ctl, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(logged("adapter: print request from 4660.9"), 1);
    m_close(&ctl);
    CHECK_EQ_INT(desc_of(nsent - 1), 0x81);
    CHECK_EQ_INT(sent[nsent - 1].to.addr.net, 0x1234);
    CHECK_EQ_INT(sent[nsent - 1].to.addr.node, 9);
    CHECK_EQ_INT(sent[nsent - 1].to.addr.sock, 0xF1);
    CHECK_EQ_INT(sent[nsent - 1].to.via, 44);
    CHECK_EQ_INT(sent[nsent - 1].to.is_long, 1);
}

// The Open Request of the call-back carries the printer's room, though the
// engine asks before the adapter knows which connection the pipe is.
static void test_call_back_tells_the_printers_room(void)
{
    mac_t ctl, pipe;
    ask_to_print(&ctl);
    room_val = 0x0E00;
    m_close(&ctl);
    CHECK_EQ_INT(desc_of(nsent - 1), 0x81);
    EXPECT_OPEN(nsent - 1, &MACD, US, 0xA002, 0x0E00, 0x81, 0);
    room_val = 0x0D00;
    run(1000);                                  // and so does its repeat
    EXPECT_OPEN(nsent - 1, &MACD, US, 0xA002, 0x0D00, 0x81, 0);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, 0));
    EXPECT_OPEN(nsent - 1, &MACD, US, 0xA002, 0x0D00, 0x82, 0x4D02);
    // A control connection opened meanwhile says the adapter's own window.
    mac_t q;
    CHECK(m_open(&q, &MAC2, US, 0x4D09));
    CHECK_EQ_INT(sent[q.mark - 1].p[10] << 8 | sent[q.mark - 1].p[11], SWA_CTL_WINDOW);
}

// ------------------------------------------------------------- busy and the questions

static void test_busy_and_the_questions(void)
{
    mac_t ctl, pipe, q;
    start_job(&ctl, &pipe);
    ASK2(&pipe, SWA_SET_STATUS, STATUS, sizeof STATUS, 0x00, 0x00);
    m_send(&pipe, 0x60, "one", 3);

    CHECK(m_open(&q, &MAC2, US, 0x4D01));       // the same ConnID as the first Mac's
    ASK2(&q, SWA_PRINT, REQ2, sizeof REQ2, 0x00, 0x01);
    CHECK_EQ_INT(n_begin, 1);                   // the printer was not asked for again
    CHECK_EQ_INT(logged("adapter: busy to 0.21"), 1);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x01);
    const uint8_t *r = m_ask(&q, SWA_JOB_USER, "\0", 1);
    uint8_t want[259];
    memset(want, 0, sizeof want);
    memcpy(want, REQ, sizeof REQ);
    CHECK_EQ_INT(q.in_n, 72);
    CHECK_EQ_MEM(r, want, 72);                  // the first Mac's request, then 00 00
    r = m_ask(&q, SWA_JOB_USER, "\0\0", 2);
    CHECK_EQ_INT(q.in_n, 72);
    CHECK_EQ_MEM(r, want, 72);
    r = m_ask(&q, SWA_JOB_STATUS, "\0", 1);
    memset(want, 0, sizeof want);
    memcpy(want, STATUS, sizeof STATUS);
    CHECK_EQ_INT(q.in_n, 259);
    CHECK_EQ_MEM(r, want, 259);
    ASK2(&q, SWA_QUEUE_POS, REQ2, sizeof REQ2, 0x00, 0x01);

    // lpstyl sleeps 15 s here without a word: its connection is still there.
    run_with(15000, &pipe, NULL);
    CHECK(!q.closed);
    ASK2(&q, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(&q);

    // The first job is undisturbed.
    CHECK_EQ_INT(swa_job(A), 2);
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    m_send(&pipe, 0x60, "two", 3);
    CHECK_EQ_INT(printer_n, 6);
    CHECK_EQ_MEM(printer, "onetwo", 6);
    CHECK_EQ_INT(n_rename, 0);

    // The same Mac asks again while its own job prints: busy too.
    CHECK(m_open(&q, &MAC, US, 0x4D05));
    ASK2(&q, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x01);
    CHECK_EQ_INT(n_begin, 1);

    // Waiting for the pipe is busy as well, and the request is there to read.
    ask_to_print(&ctl);
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    ASK2(&q, SWA_PRINT, REQ2, sizeof REQ2, 0x00, 0x01);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x01);
    r = m_ask(&q, SWA_JOB_USER, "\0", 1);
    CHECK_EQ_INT(q.in_n, 72);
    CHECK_EQ_MEM(r, REQ, sizeof REQ);
    CHECK_EQ_INT(n_begin, 1);
}

// AirPrint, the test page, cleaning or another link's Mac holds the printer.
static void test_busy_when_the_board_prints(void)
{
    mac_t q;
    fresh(0xA000);
    busy_val = 1; begin_ret = 1;
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    ASK2(&q, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x01);       // at once
    CHECK_EQ_INT(n_begin, 1);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(n_end, 0);                     // no job_end for a job_begin that said busy
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x01);
    ASK2(&q, SWA_QUEUE_POS, REQ, sizeof REQ, 0x00, 0x01);
    const uint8_t *r = m_ask(&q, SWA_JOB_USER, "\0", 1);    // not this adapter's job: zeros
    uint8_t zero[259] = { 0 };
    CHECK_EQ_INT(q.in_n, 72);
    CHECK_EQ_MEM(r, zero, 72);
    ASK2(&q, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(&q);
    run(3000);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 0);  // nobody is called
    CHECK_EQ_INT(logged("adapter: busy to 0.20"), 1);

    // It is free again: the same Mac's next try is taken.
    busy_val = 0; begin_ret = 0;
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    ASK2(&q, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(n_begin, 2);
    CHECK_EQ_INT(swa_job(A), 1);
}

static void test_idle_queries(void)
{
    mac_t q;
    uint8_t zero[259] = { 0 };
    fresh(0xA000);
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x00);
    ASK2(&q, SWA_QUEUE_POS, REQ, sizeof REQ, 0x00, 0x00);
    const uint8_t *r = m_ask(&q, SWA_JOB_USER, "\0", 1);
    CHECK_EQ_INT(q.in_n, 72);
    CHECK_EQ_MEM(r, zero, 72);
    r = m_ask(&q, SWA_JOB_STATUS, "\0", 1);
    CHECK_EQ_INT(q.in_n, 259);
    CHECK_EQ_MEM(r, zero, 259);
    // Codes nobody knows, and the ones with nothing to do, get 00 00.
    static const uint16_t CODES[] = { 0x0001, 0x0002, 0x0003, 0x0004, 0x0008, 0x000A, 0x000F,
                                      0x0042, 0xFFFF };
    for (size_t i = 0; i < sizeof CODES / sizeof CODES[0]; i++)
        ASK2(&q, CODES[i], "\0", 1, 0x00, 0x00);
    ASK2(&q, 0x0042, NULL, 0, 0x00, 0x00);
    ASK2(&q, SWA_WITHDRAW, REQ, sizeof REQ, 0x00, 0x00);    // nothing to withdraw
    ASK2(&q, SWA_DRAINED, "\0", 1, 0xFF, 0xFF);             // nothing waits here
    CHECK_EQ_INT(n_begin, 0);
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(nlog, 0);
    // A status text sent off the pipe is not kept.
    ASK2(&q, SWA_SET_STATUS, STATUS, sizeof STATUS, 0x00, 0x00);
    r = m_ask(&q, SWA_JOB_STATUS, "\0", 1);
    CHECK_EQ_MEM(r, zero, 259);
    // Stream bytes on a control connection are taken and dropped.
    m_send(&q, 0x60, "junk", 4);
    CHECK_EQ_INT(printer_n, 0);
    CHECK_EQ_INT(sent[nsent - 1].p[9], 5);      // acked: the Mac need not send them again
}

static void test_short_request(void)
{
    mac_t q;
    fresh(0xA000);
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    ASK2(&q, SWA_PRINT, REQ, 4, 0xFF, 0xFF);
    ASK2(&q, SWA_PRINT, NULL, 0, 0xFF, 0xFF);
    ASK2(&q, SWA_PRINT, "\0\0\0\0\0", 5, 0xFF, 0xFF);       // no data socket
    CHECK_EQ_INT(n_begin, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x00);           // not busy afterwards
    // Five bytes are enough; the rest reads as zeros.
    ASK2(&q, SWA_PRINT, REQ, 5, 0x00, 0x00);
    CHECK_EQ_INT(swa_job(A), 1);
    const uint8_t *r = m_ask(&q, SWA_JOB_USER, "\0", 1);
    uint8_t want[72] = { 0, 0, 0, 0xF1, 3 };
    CHECK_EQ_INT(q.in_n, 72);
    CHECK_EQ_MEM(r, want, 72);
    // More than seventy: the first seventy.
    uint8_t big[80];
    memset(big, 0x55, sizeof big);
    memcpy(big, REQ, 8);
    fresh(0xA000);
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    ASK2(&q, SWA_PRINT, big, sizeof big, 0x00, 0x00);
    r = m_ask(&q, SWA_JOB_USER, "\0", 1);
    CHECK_EQ_MEM(r, big, 70);
    CHECK_EQ_INT(r[70], 0); CHECK_EQ_INT(r[71], 0);
}

static void test_withdraw(void)
{
    mac_t ctl, q, pipe;
    ask_to_print(&ctl);
    run(2500);                                  // the call-back goes unanswered
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 2);
    CHECK(m_open(&q, &MACQ, US, 0x4D06));
    ASK2(&q, SWA_WITHDRAW, REQ2, sizeof REQ2, 0x00, 0x00);  // someone else's
    CHECK_EQ_INT(swa_job(A), 1);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x01);
    ASK2(&q, SWA_WITHDRAW, REQ, 4, 0x00, 0x00);             // too short to name a job
    CHECK_EQ_INT(swa_job(A), 1);
    CHECK_EQ_INT(n_end, 0);

    ASK2(&q, SWA_WITHDRAW, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 1);
    CHECK_EQ_INT(logged("adapter: job ended cleanly"), 1);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x00);
    int calls = count_to(&MACD, 0x81, 0);
    run_with(10000, &q, &ctl);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), calls);          // no more Open Requests
    CHECK_EQ_INT(printer_n, 0);
    CHECK_EQ_INT(n_end, 1);
    // A late answer to the call opens nothing.
    CHECK(!m_called(&pipe, &MACD, US, 0x4D02, 0));
    CHECK_EQ_INT(n_up, 0);
    // And the next request is taken.
    ASK2(&q, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(n_begin, 2);

    // Before the call-back has begun, on the connection that asked.
    ask_to_print(&ctl);
    ASK2(&ctl, SWA_WITHDRAW, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 1);
    ASK2(&ctl, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(&ctl);
    run(3000);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 0);

    // Once the job is in the pipe it is too late.
    start_job(&ctl, &pipe);
    CHECK(m_open(&q, &MACQ, US, 0x4D06));
    ASK2(&q, SWA_WITHDRAW, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(swa_job(A), 2);
    CHECK_EQ_INT(n_end, 0);
}

// ------------------------------------------------------------- a Mac that vanishes (review focus 1)

// After the accept: not a word more, not even an ack of the answer.
static void test_mac_gone_after_accept(void)
{
    mac_t ctl, q;
    ask_to_print(&ctl);
    uint64_t t0 = T, ended = 0;
    for (int i = 0; i < 300; i++) {
        T += 100;
        swa_tick(A, T);
        if (n_end && !ended) ended = T - t0;
    }
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(ended, 7000);                  // a second, then six of calling
    CHECK(ended <= SWA_PIPE_MS);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1 + ADSP_OPEN_RETRIES);
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 1);   // what it still had open is closed
    CHECK_EQ_INT(printer_n, 0);
    CHECK_EQ_INT(n_up, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(logged("adapter: job ended no pipe"), 1);
    // Not busy to the next Mac.
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x00);
    ASK2(&q, SWA_PRINT, REQ2, sizeof REQ2, 0x00, 0x00);
    CHECK_EQ_INT(n_begin, 2);
    CHECK_EQ_INT(n_end, 1);
}

// The board's clock was held up: the 15 s end stands on its own.
static void test_no_pipe_in_fifteen_seconds(void)
{
    mac_t ctl, pipe;
    ask_to_print(&ctl);
    T += SWA_PIPE_MS - 1;
    swa_tick(A, T);                             // the call, late
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    T += 1;
    swa_tick(A, T);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(count_to(&MACD, 0x85, 0), 1);  // the call is taken back
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 1);
    CHECK_EQ_INT(logged("adapter: job ended no pipe"), 1);
    CHECK(!m_called(&pipe, &MACD, US, 0x4D02, 0));          // too late
    CHECK_EQ_INT(n_up, 0);
    run(20000);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
}

static void test_mac_gone_during_call_back(void)
{
    mac_t ctl;
    ask_to_print(&ctl);
    ASK2(&ctl, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(&ctl);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    uint64_t t0 = T, ended = 0;
    for (int i = 0; i < 300; i++) {
        T += 100;
        swa_tick(A, T);
        if (n_end && !ended) ended = T - t0;
    }
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(ended, 6000);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1 + ADSP_OPEN_RETRIES);
    CHECK_EQ_INT(printer_n, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(logged("adapter: job ended no pipe"), 1);

    // The Mac says no outright: at once.
    ask_to_print(&ctl);
    m_close(&ctl);
    uint16_t ours = (uint16_t)(sent[nsent - 1].p[0] << 8 | sent[nsent - 1].p[1]);
    rx_open(&MACD, US, 0x4D02, 0x0800, 0x84, ours);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(swa_job(A), 0);
}

// With the printer's answer on its way: our resends run out.
static void test_mac_gone_mid_page(void)
{
    mac_t ctl, pipe, q;
    start_job(&ctl, &pipe);
    m_send(&pipe, 0x60, "\xFF\xFF\xFF\x31", 4);
    printer_says("\x08", 1);
    swa_tick(A, T);
    CHECK_EQ_INT(count_to(&MACD, 0x40, 0), 1);
    uint64_t t0 = T, ended = 0;
    for (int i = 0; i < 400; i++) {
        T += 100;
        swa_tick(A, T);
        if (n_end && !ended) ended = T - t0;
    }
    CHECK_EQ_INT(n_end, 1);                     // once
    CHECK_EQ_INT(end_clean, 0);                 // the board resets the printer
    CHECK_EQ_INT(ended, 18000);
    CHECK_EQ_INT(count_to(&MACD, 0x40, 0), 1 + ADSP_RESENDS);
    CHECK_EQ_INT(count_to(&MACD, 0x85, 0), 1);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(logged("adapter: job ended Mac gone"), 1);
    CHECK_EQ_INT(printer_n, 4);                 // the adapter itself sent the printer nothing
    // A new request is taken.
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    ASK2(&q, SWA_PRINT, REQ2, sizeof REQ2, 0x00, 0x00);
    CHECK_EQ_INT(n_begin, 2);
    CHECK_EQ_INT(n_end, 1);

    // A bare close, with no 0012 before it, is as unclean.
    start_job(&ctl, &pipe);
    m_send(&pipe, 0x60, "half a page", 11);
    m_close(&pipe);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(logged("adapter: job ended Mac gone"), 1);
}

// With nothing in flight: only our probes notice.
static void test_mac_gone_idle_pipe(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    m_send(&pipe, 0x60, "\xFF\xFF\xFF\x31", 4);
    uint64_t t0 = T, ended = 0;
    for (int i = 0; i < 400; i++) {
        T += 100;
        swa_tick(A, T);
        if (n_end && !ended) ended = T - t0;
    }
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(ended, 20000);
    CHECK_EQ_INT(count_to(&MACD, 0xC0, 0), ADSP_PROBE_EXPIRIES - 1);
    CHECK_EQ_INT(count_to(&MACD, 0x85, 0), 1);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(logged("adapter: job ended Mac gone"), 1);

    // A quiet Mac that answers the probes keeps the printer, ten minutes on.
    start_job(&ctl, &pipe);
    run_with(600000, &pipe, NULL);
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(swa_job(A), 2);
    m_send(&pipe, 0x60, "still here", 10);
    CHECK_EQ_INT(printer_n, 10);
    // Gone after 0012, before its close: the end is still the clean one.
    ASK2(&pipe, SWA_END, "\0", 1, 0xFF, 0xFF);
    run(25000);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 1);
}

// ------------------------------------------------------------- attentions (review focus 2)

static void test_repeated_attention_is_answered_once(void)
{
    mac_t ctl, pipe;
    fresh(0xA000);
    CHECK(m_open(&ctl, &MAC, US, 0x4D01));
    int mark = nsent;
    rx_attn(&MAC, US, 0x4D01, 0, 0x50, SWA_PRINT, REQ, sizeof REQ);
    rx_attn(&MAC, US, 0x4D01, 0, 0x50, SWA_PRINT, REQ, sizeof REQ);     // its ack was lost
    rx_attn(&MAC, US, 0x4D01, 0, 0x50, SWA_PRINT, REQ, sizeof REQ);
    CHECK_EQ_INT(count_to(&MAC, 0x90, mark), 3);            // acked each time
    CHECK_EQ_INT(count_to(&MAC, 0x40, mark), 1);            // answered once
    CHECK_EQ_INT(n_begin, 1);
    CHECK_EQ_INT(swa_job(A), 1);
    ctl.attn = 1;
    m_poll(&ctl);
    CHECK_EQ_INT(ctl.in_n, 2);
    CHECK_EQ_MEM(ctl.in, "\0\0", 2);            // accepted, not busy to itself
    m_ack(&ctl);
    ASK2(&ctl, SWA_END, "\0", 1, 0x00, 0x00);
    mark = nsent;
    rx_attn(&MAC, US, 0x4D01, 1, 0x50, SWA_END, "\0", 1);   // 0012 again
    CHECK_EQ_INT(count_to(&MAC, 0x90, mark), 1);
    CHECK_EQ_INT(nsent, mark + 1);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);              // and one call
    m_close(&ctl);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, 0));

    // On the pipe, with the stream in use: neither its acks nor its window move.
    m_send(&pipe, 0x60, "abc", 3);              // M 0 -> 4
    printer_says("\x08", 1);
    swa_tick(A, T);
    m_poll(&pipe);
    m_ack(&pipe);
    mark = nsent;
    // The attention's sequence fields would, read as the stream's, ack bytes
    // never sent and shut the window.
    rx(&MACD, US, 0x4D02, pipe.attn, 99, 0, 0x50, "\x00\x06\x00", 3);
    rx(&MACD, US, 0x4D02, pipe.attn, 99, 0, 0x50, "\x00\x06\x00", 3);
    pipe.attn++;
    CHECK_EQ_INT(count_to(&MACD, 0x90, mark), 2);
    CHECK_EQ_INT(count_to(&MACD, 0x40, mark), 1);
    EXPECT(mark + 1, &MACD, US, pipe.far, 1, 4, 0x1000, 0x40, "\xFF\xFF", 2);
    printer_says("\x09", 1);                    // the printer's next byte still goes out
    swa_tick(A, T);
    EXPECT(nsent - 1, &MACD, US, pipe.far, 3, 4, 0x1000, 0x40, "\x09", 1);
    m_send(&pipe, 0x60, "def", 3);              // and the Mac's still comes in
    CHECK_EQ_INT(printer_n, 6);
    CHECK_EQ_MEM(printer, "abcdef", 6);
    EXPECT(nsent - 1, &MACD, US, pipe.far, 4, 8, 0x1000, 0x80, NULL, 0);
}

static void test_fast_modes_refused(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    ASK2(&pipe, SWA_FAST_CLOCK, "\0", 1, 0x00, 0xEF);       // the Mac reads the low byte
    ASK2(&pipe, SWA_FAST_SERIAL, "\0\0", 2, 0xFF, 0xEF);
    ASK2(&pipe, SWA_FAST_SERIAL, "\0\1", 2, 0xFF, 0xEF);
    ASK2(&pipe, SWA_SERIAL_CONFIG, "\x4C\0", 2, 0x00, 0x00);
    ASK2(&pipe, SWA_SERIAL_OPTIONS, "\x40", 1, 0x00, 0x00);
    ASK2(&pipe, SWA_SERIAL_LOWER, "\1\1", 2, 0x00, 0x00);
    CHECK_EQ_INT(printer_n, 0);
    CHECK_EQ_INT(swa_job(A), 2);
    // Asked on a control connection, the same.
    mac_t q;
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    ASK2(&q, SWA_FAST_CLOCK, "\0", 1, 0x00, 0xEF);
    ASK2(&q, SWA_FAST_SERIAL, "\0\0", 2, 0xFF, 0xEF);
}

static void test_lone_switch_byte_is_dropped(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    // D.8's four: 'x' alone, 'h' alone, "xx", and an 'x' that ends a message
    // an earlier packet began.
    m_send(&pipe, 0x60, "x", 1);
    CHECK_EQ_INT(printer_n, 0);
    m_send(&pipe, 0x60, "h", 1);
    CHECK_EQ_INT(printer_n, 0);
    m_send(&pipe, 0x60, "xx", 2);
    CHECK_EQ_INT(printer_n, 2);
    m_send(&pipe, 0x40, "ab", 2);
    m_send(&pipe, 0x60, "x", 1);
    CHECK_EQ_INT(printer_n, 5);
    CHECK_EQ_MEM(printer, "xxabx", 5);
    // Dropped bytes are acked like any others: M is 2 + 2 + 3 + 4.
    EXPECT(nsent - 1, &MACD, US, pipe.far, 0, 11, 0x1000, 0x80, NULL, 0);

    // The byte, then its end of message in a packet of its own: still alone.
    // While it is held the window keeps a place for it.
    m_send(&pipe, 0x40, "x", 1);
    CHECK_EQ_INT(printer_n, 5);
    EXPECT(nsent - 1, &MACD, US, pipe.far, 0, 12, 0x0FFF, 0x80, NULL, 0);
    m_send(&pipe, 0x60, NULL, 0);
    CHECK_EQ_INT(printer_n, 5);
    EXPECT(nsent - 1, &MACD, US, pipe.far, 0, 13, 0x1000, 0x80, NULL, 0);
    // The byte, then more of its message: data, in order.
    m_send(&pipe, 0x40, "h", 1);
    CHECK_EQ_INT(printer_n, 5);
    m_send(&pipe, 0x60, "i", 1);
    CHECK_EQ_INT(printer_n, 7);
    m_send(&pipe, 0x40, "x", 1);
    m_send(&pipe, 0x40, "x", 1);
    m_send(&pipe, 0x60, NULL, 0);
    CHECK_EQ_INT(printer_n, 9);
    // A bare end of message between messages is nothing.
    m_send(&pipe, 0x60, NULL, 0);
    m_send(&pipe, 0x60, "x", 1);
    CHECK_EQ_INT(printer_n, 9);
    // Other single bytes pass, and a message after a dropped one is whole.
    m_send(&pipe, 0x60, "?", 1);
    m_send(&pipe, 0x60, "X", 1);
    m_send(&pipe, 0x60, "xyz", 3);
    CHECK_EQ_INT(printer_n, 14);
    CHECK_EQ_MEM(printer, "xxabxhixx?Xxyz", 14);
    CHECK_EQ_INT(n_end, 0);

    // A byte held when the Mac goes is not sent after it.
    m_send(&pipe, 0x40, "x", 1);
    m_close(&pipe);
    CHECK_EQ_INT(printer_n, 14);
    CHECK_EQ_INT(n_end, 1);
    // And the next job starts at a message boundary with nothing held.
    CHECK(m_open(&ctl, &MAC, US, 0x4D01));
    ASK2(&ctl, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    int mark = nsent;
    m_close(&ctl);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, mark));
    m_send(&pipe, 0x60, "x", 1);
    m_send(&pipe, 0x60, "y", 1);
    CHECK_EQ_INT(printer_n, 15);
    CHECK_EQ_INT(printer[14], 'y');
}

static void test_output_drained(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    uint8_t page[2048];
    for (size_t i = 0; i < sizeof page; i++) page[i] = (uint8_t)(i * 7);
    drained = 0;                                // the port is slow
    for (size_t at = 0; at < sizeof page; at += 512) m_send(&pipe, 0x40, page + at, 512);
    ASK2(&pipe, SWA_DRAINED, "\0", 1, 0x00, 0x00);
    ASK2(&pipe, SWA_DRAINED, "\0", 1, 0x00, 0x00);
    drained = 1;                                // it has taken them
    ASK2(&pipe, SWA_DRAINED, "\0", 1, 0xFF, 0xFF);
    CHECK_EQ_INT(printer_n, sizeof page);
    CHECK_EQ_MEM(printer, page, sizeof page);
}

// ------------------------------------------------------------- the pipe's two directions

// The window is the room in front of the printer: it shuts as that fills and
// is said again, unasked, when it reopens. Nothing is lost or reordered.
static void test_pipe_window_follows_the_printer(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    sink_cap = 1000;
    uint8_t page[3000];
    for (size_t i = 0; i < sizeof page; i++) page[i] = (uint8_t)(i * 13 + 1);
    size_t at = 0;
    int unasked = 0;
    for (int round = 0; round < 200 && at < sizeof page; round++) {
        // The Mac sends what the last window allows, 500 at most a packet.
        const sent_t *s = &sent[nsent - 1];
        size_t win = (size_t)(s->p[10] << 8 | s->p[11]);
        uint32_t acked = (uint32_t)s->p[6] << 24 | (uint32_t)s->p[7] << 16 |
                         (uint32_t)s->p[8] << 8 | s->p[9];
        CHECK(to_route(nsent - 1, &MACD));
        at = acked;
        pipe.tx = acked;
        if (!win) {                             // shut: the printer takes what it holds
            CHECK_EQ_INT(sink_held, sink_cap);
            int before = nsent;
            sink_held = 0;
            swa_tick(A, T);
            CHECK_EQ_INT(nsent, before + 1);    // the news, with no word from the Mac
            CHECK_EQ_INT(desc_of(before), 0x80);
            unasked++;
            continue;
        }
        size_t n = sizeof page - at;
        if (n > win) n = win;
        if (n > 500) n = 500;
        m_send(&pipe, 0x40, page + at, n);
    }
    CHECK_EQ_INT(at, sizeof page);
    CHECK_EQ_INT(printer_n, sizeof page);
    CHECK_EQ_MEM(printer, page, sizeof page);
    CHECK_EQ_INT(unasked, 3);                 // at 1000, 2000 and 3000
}

// The printer's bytes go back as they come, and never crowd out an answer.
static void test_printer_bytes_leave_room_for_answers(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    uint8_t chatter[2000];
    for (size_t i = 0; i < sizeof chatter; i++) chatter[i] = (uint8_t)(i * 3 + 5);
    printer_says(chatter, sizeof chatter);
    swa_tick(A, T);                             // the Mac acks none of it
    CHECK_EQ_INT(out_at, ADSP_TX_CAP - SWA_ANSWER_ROOM);
    CHECK_EQ_INT(adsp_tx_free(&A->adsp, A->pipe), SWA_ANSWER_ROOM);
    run(500);
    CHECK_EQ_INT(out_at, ADSP_TX_CAP - SWA_ANSWER_ROOM);
    int mark = nsent;
    rx_attn(&MACD, US, 0x4D02, pipe.attn++, 0x50, SWA_DRAINED, "\0", 1);
    CHECK_EQ_INT(count_to(&MACD, 0x40, mark), 1);           // answered all the same
    CHECK_EQ_INT(sent[nsent - 1].n, ADSP_HDR + 2);
    CHECK_EQ_MEM(sent[nsent - 1].p + ADSP_HDR, "\xFF\xFF", 2);
    // As the Mac reads, the rest follows in order.
    for (int i = 0; i < 20; i++) {
        m_poll(&pipe);
        m_ack(&pipe);
        swa_tick(A, T);
    }
    m_poll(&pipe);
    CHECK_EQ_INT(out_at, sizeof chatter);
    CHECK_EQ_INT(pipe.in_n, sizeof chatter + 2);
    CHECK_EQ_MEM(pipe.in, chatter, ADSP_TX_CAP - SWA_ANSWER_ROOM);
    CHECK_EQ_MEM(pipe.in + ADSP_TX_CAP - SWA_ANSWER_ROOM, "\xFF\xFF", 2);
    CHECK_EQ_MEM(pipe.in + ADSP_TX_CAP - SWA_ANSWER_ROOM + 2,
                 chatter + ADSP_TX_CAP - SWA_ANSWER_ROOM,
                 sizeof chatter - (ADSP_TX_CAP - SWA_ANSWER_ROOM));

    // With no job in the pipe the printer's bytes are not ours to move.
    mac_t q;
    fresh(0xA000);
    printer_says("stray", 5);
    run(1000);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(out_at, 0);
    ask_to_print(&q);
    printer_says("stray", 5);
    run(500);                                   // nor while it waits for the pipe
    CHECK_EQ_INT(out_at, 0);
}

// A Mac that asks and asks and never reads its answers is not kept.
static void test_mac_that_reads_nothing_is_dropped(void)
{
    mac_t q;
    fresh(0xA000);
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    for (int i = 0; i < 2; i++) rx_attn(&MAC, US, 0x4D01, q.attn++, 0x50, SWA_JOB_STATUS, "\0", 1);
    CHECK(adsp_is_open(&A->adsp, 0));
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 0);
    rx_attn(&MAC, US, 0x4D01, q.attn++, 0x50, SWA_JOB_STATUS, "\0", 1);  // 777 > 640
    CHECK(!adsp_is_open(&A->adsp, 0));
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 1);
    CHECK(m_open(&q, &MAC, US, 0x4D02));        // its place is free

    // The same on the connection a request came on: the call-back follows.
    mac_t ctl;
    ask_to_print(&ctl);
    for (int i = 0; i < 3; i++)
        rx_attn(&MAC, US, 0x4D01, ctl.attn++, 0x50, SWA_JOB_STATUS, "\0", 1);
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 1);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 1);
    CHECK_EQ_INT(swa_job(A), 1);
    CHECK_EQ_INT(n_end, 0);
}

// ------------------------------------------------------------- jobs, names, places

static void test_two_jobs_in_a_row(void)
{
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    uint16_t first_pipe = pipe.far;
    ASK2(&pipe, SWA_SET_STATUS, STATUS, sizeof STATUS, 0x00, 0x00);
    m_send(&pipe, 0x60, "first", 5);
    ASK2(&pipe, SWA_END, "\0", 1, 0xFF, 0xFF);
    m_close(&pipe);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 1);

    // The same Mac, the same connection IDs.
    int mark = nsent;
    CHECK(m_open(&ctl, &MAC, US, 0x4D01));
    ASK2(&ctl, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    ASK2(&ctl, SWA_END, "\0", 1, 0x00, 0x00);
    m_close(&ctl);
    CHECK(m_called(&pipe, &MACD, US, 0x4D02, mark));
    CHECK(pipe.far != first_pipe);
    m_send(&pipe, 0x60, "second", 6);
    ASK2(&pipe, SWA_END, "\0", 1, 0xFF, 0xFF);
    m_close(&pipe);
    CHECK_EQ_INT(n_begin, 2);
    CHECK_EQ_INT(n_up, 2);
    CHECK_EQ_INT(n_end, 2);
    CHECK_EQ_INT(end_clean, 1);
    CHECK_EQ_INT(printer_n, 11);
    CHECK_EQ_MEM(printer, "firstsecond", 11);
}

// The request holds a person's name and the status text their words: both
// are gone with the job, however it ended.
static void test_request_and_status_are_forgotten(void)
{
    static const uint8_t zero[sizeof A->status] = { 0 };
    for (int how = 0; how < 6; how++) {
        mac_t ctl, pipe, q;
        if (how == 5) {
            ask_to_print(&ctl);                 // never reaches the pipe
        } else {
            start_job(&ctl, &pipe);
            ASK2(&pipe, SWA_SET_STATUS, STATUS, sizeof STATUS, 0x00, 0x00);
            CHECK(memcmp(A->status, zero, sizeof A->status) != 0);
        }
        CHECK(memcmp(A->req, zero, sizeof A->req) != 0);
        switch (how) {
        case 0: ASK2(&pipe, SWA_END, "\0", 1, 0xFF, 0xFF); m_close(&pipe); break;
        case 1: m_close(&pipe); break;
        case 2: swa_stop(A, T); break;
        case 3: swa_reset(A); swa_offer(A, NAMES, 1, T); break;
        case 4: swa_offer(A, NULL, 0, T); swa_offer(A, NAMES, 1, T); break;
        case 5: run(8000); break;
        }
        CHECK_EQ_INT(n_end, 1);
        CHECK_EQ_INT(swa_job(A), 0);
        CHECK_EQ_MEM(A->req, zero, sizeof A->req);
        CHECK_EQ_MEM(A->status, zero, sizeof A->status);
        CHECK_EQ_INT(A->mac.addr.node, 0);
        CHECK(m_open(&q, &MAC2, US, 0x4D01));
        const uint8_t *r = m_ask(&q, SWA_JOB_USER, "\0", 1);
        CHECK_EQ_INT(q.in_n, 72);
        CHECK_EQ_MEM(r, zero, 72);
        r = m_ask(&q, SWA_JOB_STATUS, "\0", 1);
        CHECK_EQ_INT(q.in_n, 259);
        CHECK_EQ_MEM(r, zero, 259);
    }
    // A new request clears the last job's status text from the first.
    mac_t ctl, pipe;
    start_job(&ctl, &pipe);
    uint8_t odd[SWA_STATUS_LEN];
    memset(odd, 'Z', sizeof odd);               // a length byte that overruns
    odd[0] = 0x06; odd[1] = 0x47; odd[2] = 0xFF;
    ASK2(&pipe, SWA_SET_STATUS, odd, sizeof odd, 0x00, 0x00);
    const uint8_t *r = m_ask(&pipe, SWA_JOB_STATUS, "\0", 1);
    CHECK_EQ_INT(pipe.in_n, 259);
    CHECK_EQ_INT(r[2], 254);                    // cut to what came
    CHECK_EQ_INT(r[256], 'Z'); CHECK_EQ_INT(r[257], 0); CHECK_EQ_INT(r[258], 0);
    ASK2(&pipe, SWA_SET_STATUS, "\x06\x47\x09" "ab", 5, 0x00, 0x00);    // a short one
    r = m_ask(&pipe, SWA_JOB_STATUS, "\0", 1);
    CHECK_EQ_INT(r[2], 2);
    CHECK_EQ_INT(r[5], 0);
}

static void test_offer_withdrawn_mid_job_stops_it(void)
{
    mac_t ctl, pipe, q;
    start_job(&ctl, &pipe);
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    m_send(&pipe, 0x60, "half", 4);

    // The same record under another name: nothing stops.
    nbp_name(NAMES[0].obj, "Inkwell too");
    swa_offer(A, NAMES, 1, T);
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(swa_job(A), 2);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x01);

    swa_offer(A, NAMES, 0, T);                  // an ImageWriter is on the port now
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(count_to(&MACD, 0x85, 0), 1);  // the pipe is closed
    CHECK_EQ_INT(count_to(&MAC2, 0x85, 0), 1);  // and the connections on that name
    CHECK_EQ_INT(logged("adapter: job ended withdrawn"), 1);
    // Nothing more reaches the printer, and nobody is answered there.
    m_send(&pipe, 0x60, "more", 4);
    CHECK_EQ_INT(printer_n, 4);
    int mark = nsent;
    CHECK(!m_open(&q, &MAC2, US, 0x4D03));
    rx_attn(&MAC2, US, 0x4D01, 1, 0x50, SWA_PRINT, REQ2, sizeof REQ2);
    CHECK_EQ_INT(nsent, mark);
    CHECK_EQ_INT(n_begin, 1);
    swa_offer(A, NAMES, 0, T);                  // said again: nothing more
    run(30000);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(nsent, mark);

    // The name's record changes under a job that waits for its pipe.
    ask_to_print(&ctl);
    NAMES[0].id = 4;
    swa_offer(A, NAMES, 1, T);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(count_to(&MAC, 0x85, 0), 1);
    run(3000);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), 0);  // no call-back
    CHECK(m_open(&q, &MAC2, US, 0x4D01));       // the new record is offered
    ASK2(&q, SWA_PRINT, REQ2, sizeof REQ2, 0x00, 0x00);
    CHECK_EQ_INT(begin_id, 4);

    // A request that races the swap is refused, never taken.
    fresh(0xA000);
    CHECK(m_open(&q, &MAC, US, 0x4D01));
    A->names[0].id = 0;                         // as if the name went under an open connection
    ASK2(&q, SWA_PRINT, REQ, sizeof REQ, 0xFF, 0xFF);
    CHECK_EQ_INT(n_begin, 0);

    // The other name's job and connections are left alone.
    fresh(0xA000);
    nbp_name(NAMES[1].obj, "Second");
    nbp_name(NAMES[1].type, SWA_TYPE_MONO);
    NAMES[1].id = 5;
    swa_offer(A, NAMES, 2, T);
    CHECK(m_open(&ctl, &MAC, US2, 0x4D01));
    ASK2(&ctl, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(begin_id, 5);
    mark = nsent;
    m_close(&ctl);
    CHECK_EQ_INT(sent[nsent - 1].sock, US2);    // the call comes from that name's socket
    CHECK(m_called(&pipe, &MACD, US2, 0x4D02, mark));
    NAMES[0].id = 0;
    swa_offer(A, NAMES, 2, T);
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(swa_job(A), 2);
    swa_offer(A, NAMES, 1, T);
    CHECK_EQ_INT(n_end, 1);
}

static void test_fourth_control_connection_is_not_answered(void)
{
    mac_t ctl, pipe, m[4];
    start_job(&ctl, &pipe);
    for (int i = 0; i < 3; i++) {
        lt_route_t r = { { 0, (uint8_t)(30 + i), 0xE0 }, (uint8_t)(30 + i), 0 };
        CHECK(m_open(&m[i], &r, US, 0x4D01));
    }
    static const lt_route_t FOURTH = { { 0, 33, 0xE0 }, 33, 0 };
    int mark = nsent;
    CHECK(!m_open(&m[3], &FOURTH, US, 0x4D01));
    rx_attn(&FOURTH, US, 0x4D01, 0, 0x50, SWA_JOB_COUNT, "\0", 1);
    CHECK_EQ_INT(nsent, mark);                  // silence: its driver tries again
    m_close(&m[1]);
    CHECK(m_open(&m[3], &FOURTH, US, 0x4D01));  // and gets in when one has gone
    ASK2(&m[3], SWA_JOB_COUNT, "\0", 1, 0x00, 0x01);
    CHECK_EQ_INT(swa_job(A), 2);

    // With no job, three too: the fourth place is the pipe's, so a request
    // from any of the three can always be called back.
    fresh(0xA000);
    for (int i = 0; i < 3; i++) {
        lt_route_t r = { { 0, (uint8_t)(30 + i), 0xE0 }, (uint8_t)(30 + i), 0 };
        CHECK(m_open(&m[i], &r, US, 0x4D01));
    }
    CHECK(!m_open(&m[3], &FOURTH, US, 0x4D01));
    memcpy(REQ, "\x00\x00\x00\xF1", 4);
    ASK2(&m[2], SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    mark = nsent;
    ASK2(&m[2], SWA_END, "\0", 1, 0x00, 0x00);
    static const lt_route_t M2D = { { 0, 32, 0xF1 }, 32, 0 };
    CHECK(m_called(&pipe, &M2D, US, 0x4D02, mark));
    CHECK_EQ_INT(swa_job(A), 2);
    CHECK(!m_open(&m[3], &FOURTH, US, 0x4D01)); // still three beside the pipe
}

static void test_lookup(void)
{
    fresh(0xA000);
    nbp_name(NAMES[1].obj, "Second");
    nbp_name(NAMES[1].type, SWA_TYPE_MONO);
    NAMES[1].id = 5;
    swa_offer(A, NAMES, 2, T);
    nbp_tuple_t q;
    const swa_name_t *hit = NULL;
    memset(&q, 0, sizeof q);
    nbp_name(q.obj, "=");
    nbp_name(q.type, "=");
    nbp_name(q.zone, "*");
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), 0);
    CHECK(hit && hit->id == 3);
    CHECK_EQ_INT(swa_lookup(A, &q, 1, &hit), 1);
    CHECK(hit && hit->id == 5 && !memcmp(hit->obj, NAMES[1].obj, sizeof hit->obj));
    CHECK_EQ_INT(swa_lookup(A, &q, 2, &hit), -1);
    CHECK_EQ_INT(swa_lookup(A, &q, 0, NULL), 0);
    nbp_name(q.type, "stylewriter1200at");      // by type, case folded
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), 1);
    CHECK_EQ_INT(swa_lookup(A, &q, 2, &hit), -1);
    nbp_name(q.type, SWA_TYPE_COLOR);
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), 0);
    CHECK_EQ_INT(swa_lookup(A, &q, 1, &hit), -1);
    nbp_name(q.obj, "Inkwell");
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), 0);
    nbp_name(q.obj, "Second");
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), -1);           // the wrong type for that name
    nbp_name(q.type, "LaserWriter");
    nbp_name(q.obj, "=");
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), -1);
    // A name taken away is not answered for.
    nbp_name(q.type, "=");
    swa_offer(A, NAMES, 1, T);
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), 0);
    CHECK_EQ_INT(swa_lookup(A, &q, 1, &hit), -1);
    swa_offer(A, NULL, 0, T);
    CHECK_EQ_INT(swa_lookup(A, &q, 0, &hit), -1);
}

static void test_only_our_sockets_and_adsp(void)
{
    uint8_t p[ADSP_HDR + 8];
    uint8_t par[8] = { 0x01, 0x00, 0, 0, 0, 0, 0, 0 };
    size_t n = pk(p, 0x4D01, 0, 0, 0x0800, 0x81, par, 8);
    fresh(0xA000);
    wire(&MAC, 0x81, DDP_ADSP, p, n);           // the ImageWriter's listener is not ours
    wire(&MAC, 0x88, DDP_ADSP, p, n);
    wire(&MAC, 0x8B, DDP_ADSP, p, n);
    wire(&MAC, US, DDP_ATP, p, n);              // not ADSP
    wire(&MAC, US2, DDP_ADSP, p, n);            // a socket with no name on it
    CHECK_EQ_INT(nsent, 0);
    wire(&MAC, US, DDP_ADSP, p, 5);             // too short to be a packet
    CHECK_EQ_INT(nsent, 0);
    wire(&MAC, US, DDP_ADSP, p, n);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(desc_of(0), 0x83);
    CHECK_EQ_INT(sent[0].sock, US);
}

static void test_stop_and_reset(void)
{
    mac_t ctl, pipe, q;
    // The board ends the job (the test button): the pipe is closed, unclean.
    start_job(&ctl, &pipe);
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    swa_stop(A, T);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    CHECK_EQ_INT(count_to(&MACD, 0x85, 0), 1);
    CHECK_EQ_INT(logged("adapter: job ended stopped"), 1);
    swa_stop(A, T);                             // once
    CHECK_EQ_INT(n_end, 1);
    m_send(&pipe, 0x60, "late", 4);
    CHECK_EQ_INT(printer_n, 0);
    ASK2(&q, SWA_JOB_COUNT, "\0", 1, 0x00, 0x00);           // other Macs stay
    ASK2(&q, SWA_PRINT, REQ2, sizeof REQ2, 0x00, 0x00);
    CHECK_EQ_INT(n_begin, 2);

    // While it waits for the pipe: the call is taken back.
    ask_to_print(&ctl);
    run(1000);
    swa_stop(A, T);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    int calls = count_to(&MACD, 0x81, 0);
    run(5000);
    CHECK_EQ_INT(count_to(&MACD, 0x81, 0), calls);
    CHECK(!m_called(&pipe, &MACD, US, 0x4D02, 0));
    // With no job: nothing.
    fresh(0xA000);
    swa_stop(A, T);
    swa_reset(A);
    CHECK_EQ_INT(n_end, 0);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(nlog, 0);

    // The node was re-made: every connection forgotten, nothing sent, the
    // job ended unclean, the names kept.
    start_job(&ctl, &pipe);
    CHECK(m_open(&q, &MAC2, US, 0x4D01));
    int mark = nsent;
    swa_reset(A);
    CHECK_EQ_INT(nsent, mark);
    CHECK_EQ_INT(n_end, 1);
    CHECK_EQ_INT(end_clean, 0);
    CHECK_EQ_INT(swa_job(A), 0);
    m_send(&pipe, 0x60, "late", 4);
    rx_attn(&MAC2, US, 0x4D01, 0, 0x50, SWA_JOB_COUNT, "\0", 1);
    CHECK_EQ_INT(nsent, mark);
    CHECK_EQ_INT(printer_n, 0);
    for (int i = 0; i < 3; i++) {               // and every place is free again
        lt_route_t r = { { 0, (uint8_t)(30 + i), 0xE0 }, (uint8_t)(30 + i), 0 };
        CHECK(m_open(&q, &r, US, 0x4D01));
    }
    ASK2(&q, SWA_PRINT, REQ, sizeof REQ, 0x00, 0x00);
    CHECK_EQ_INT(n_begin, 2);
}

int main(void)
{
    test_rename_as_a_real_mac_did_it();
    test_rename_needs_its_own_session_end();
    test_whole_print_in_apples_order();
    test_lpstyls_order();
    test_call_back_triggers();
    test_call_back_tells_the_printers_room();
    test_busy_and_the_questions();
    test_busy_when_the_board_prints();
    test_idle_queries();
    test_short_request();
    test_withdraw();
    test_mac_gone_after_accept();
    test_no_pipe_in_fifteen_seconds();
    test_mac_gone_during_call_back();
    test_mac_gone_mid_page();
    test_mac_gone_idle_pipe();
    test_repeated_attention_is_answered_once();
    test_fast_modes_refused();
    test_lone_switch_byte_is_dropped();
    test_output_drained();
    test_pipe_window_follows_the_printer();
    test_printer_bytes_leave_room_for_answers();
    test_mac_that_reads_nothing_is_dropped();
    test_two_jobs_in_a_row();
    test_request_and_status_are_forgotten();
    test_offer_withdrawn_mid_job_stops_it();
    test_fourth_control_connection_is_not_answered();
    test_lookup();
    test_only_our_sockets_and_adsp();
    test_stop_and_reset();
    free(A);
    free(sent);
    TEST_END;
}
