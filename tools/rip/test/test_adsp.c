// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ADSP engine (W-1193), driven with a fake clock and a scripted Mac at
// node 20. Every packet the engine sends is captured and held to its bytes.
// The made-up values are the protocol brief's: the Mac's control connection
// 4D01 and data connection 4D02, window 0800; ours A001 and A003, window 1000;
// the Mac's data socket F1.
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "adsp.h"
#include <stdlib.h>

#define US   0x89                 // our listening socket
static const lt_route_t MAC  = { { 0, 20, 0xE0 }, 20, 0 };   // its control socket
static const lt_route_t MACD = { { 0, 20, 0xF1 }, 20, 0 };   // its data socket
static const lt_route_t MAC2 = { { 0, 21, 0xE0 }, 21, 0 };   // another Mac

// ------------------------------------------------------------- the wire

#define MAXSENT 2048
typedef struct {
    lt_route_t to;
    uint8_t    sock;
    size_t     n;
    uint8_t    p[ADSP_HDR + ADSP_MAX_DATA];
} sent_t;
static sent_t  *sent;
static int      nsent;
static adsp_t  *E;
static uint64_t T;

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

static int accept_ok, n_accept;
static int f_accept(void *ctx, uint8_t local_sock, const lt_route_t *from)
{
    (void)ctx; (void)from;
    n_accept++;
    return accept_ok && local_sock == US;
}

static int n_opened, last_opened;
static void f_opened(void *ctx, int c) { (void)ctx; n_opened++; last_opened = c; }

static int n_closed, last_closed, last_why, reopen_on_close, reopened;
static uint8_t closed_sock, closed_node;
static void f_closed(void *ctx, int c, adsp_end_t why)
{
    (void)ctx;
    n_closed++; last_closed = c; last_why = (int)why;
    closed_sock = adsp_sock(E, c);
    closed_node = adsp_peer(E, c) ? adsp_peer(E, c)->addr.node : 0;
    if (reopen_on_close) reopened = adsp_open(E, US, &MACD, T);
}

static int      n_attn, attn_conn, answer_attn;
static uint16_t attn_code;
static uint8_t  attn_body[ADSP_ATTN_MAX + 8];
static size_t   attn_n;
static void f_attention(void *ctx, int c, uint16_t code, const uint8_t *body, size_t n)
{
    (void)ctx;
    n_attn++; attn_conn = c; attn_code = code; attn_n = n;
    if (n <= sizeof attn_body) memcpy(attn_body, body, n);
    if (answer_attn) CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"\0\0", 2, T), 2);
}

// The taker: a fixed room, or a buffer of sink_cap that held bytes fill.
static uint8_t got[32 * 1024];
static size_t  got_n, room_val, sink_cap, held;
static int     n_data, n_eom, data_conn;
static size_t now_room(void) { return sink_cap ? sink_cap - held : room_val; }
static void f_data(void *ctx, int c, const uint8_t *d, size_t n, int eom)
{
    (void)ctx;
    CHECK(n <= now_room());
    CHECK(n || eom);
    n_data++; data_conn = c;
    if (eom) n_eom++;
    if (got_n + n <= sizeof got) { memcpy(got + got_n, d, n); got_n += n; }
    if (sink_cap) held += n;
}
static size_t f_room(void *ctx, int c) { (void)ctx; (void)c; return now_room(); }

static const adsp_ops_t OPS = { NULL, f_send, f_accept, f_opened, f_closed, f_attention, f_data,
                                f_room };

static void fresh(uint32_t seed)
{
    if (!E) E = malloc(sizeof *E);
    if (!sent) sent = malloc(MAXSENT * sizeof *sent);
    adsp_init(E, &OPS, seed);
    nsent = 0; T = 1000;
    accept_ok = 1; n_accept = 0;
    n_opened = 0; last_opened = -1;
    n_closed = 0; last_closed = -1; last_why = -1; reopen_on_close = 0; reopened = -1;
    n_attn = 0; attn_conn = -1; answer_attn = 0; attn_code = 0; attn_n = 0;
    got_n = 0; room_val = 0x1000; sink_cap = 0; held = 0; n_data = 0; n_eom = 0; data_conn = -1;
}

static size_t pk(uint8_t *o, uint16_t id, uint32_t first, uint32_t next, uint16_t win,
                 uint8_t desc, const void *d, size_t n)
{
    adsp_hdr_t h = { id, first, next, win, desc };
    adsp_hdr_build(o, &h);
    if (n) memcpy(o + ADSP_HDR, d, n);
    return ADSP_HDR + n;
}

// The Mac sends us a packet, to socket US, at T.
static void rx(const lt_route_t *from, uint16_t id, uint32_t first, uint32_t next, uint16_t win,
               uint8_t desc, const void *d, size_t n)
{
    uint8_t p[ADSP_HDR + 700];
    adsp_rx(E, US, from, p, pk(p, id, first, next, win, desc, d, n), T);
}

// An open packet: version, the destination ConnID, the attention sequence.
static void rx_open(const lt_route_t *from, uint16_t id, uint16_t win, uint8_t desc, uint16_t dest)
{
    uint8_t par[8] = { 0x01, 0x00, (uint8_t)(dest >> 8), (uint8_t)dest, 0, 0, 0, 0 };
    rx(from, id, 0, 0, win, desc, par, sizeof par);
}

// Packet i we sent is exactly this, to that route, from socket US.
#define EXPECT(i, to_, id, first, next, win, desc, d_, len_) do { \
    uint8_t _w[ADSP_HDR + 700]; \
    size_t _wn = pk(_w, id, first, next, win, desc, d_, len_); \
    CHECK((i) < nsent); \
    if ((i) < nsent) { \
        CHECK_EQ_INT(sent[i].n, _wn); \
        if (sent[i].n == _wn) CHECK_EQ_MEM(sent[i].p, _w, _wn); \
        CHECK_EQ_INT(sent[i].to.addr.node, (to_)->addr.node); \
        CHECK_EQ_INT(sent[i].to.addr.sock, (to_)->addr.sock); \
        CHECK_EQ_INT(sent[i].to.via, (to_)->via); \
        CHECK_EQ_INT(sent[i].sock, US); \
    } } while (0)

#define EXPECT_OPEN(i, to_, id, win, desc, dest) do { \
    uint8_t _par[8] = { 0x01, 0x00, (uint8_t)((dest) >> 8), (uint8_t)(dest), 0, 0, 0, 0 }; \
    EXPECT(i, to_, id, 0, 0, win, desc, _par, 8); } while (0)

static uint8_t desc_of(int i) { return sent[i].p[12]; }
static int count_desc(int from, uint8_t desc)
{
    int k = 0;
    for (int i = from; i < nsent; i++) if (desc_of(i) == desc) k++;
    return k;
}

// The Mac opens its control connection to us: ours is A001, c is returned.
static int mac_opens(uint16_t mac_win)
{
    fresh(0xA000);
    rx_open(&MAC, 0x4D01, mac_win, 0x81, 0);
    CHECK_EQ_INT(n_opened, 1);
    rx_open(&MAC, 0x4D01, mac_win, 0x82, 0xA001);
    CHECK_EQ_INT(nsent, 1);
    nsent = 0;
    return last_opened;
}

// We call the Mac back on F1: ours is A003, the Mac's 4D02.
static int we_open(uint16_t mac_win)
{
    fresh(0xA002);
    int c = adsp_open(E, US, &MACD, T);
    CHECK(c >= 0);
    rx_open(&MACD, 0x4D02, mac_win, 0x83, 0xA003);
    CHECK_EQ_INT(n_opened, 1);
    CHECK_EQ_INT(last_opened, c);
    CHECK_EQ_INT(nsent, 2);
    nsent = 0;
    return c;
}

// ------------------------------------------------------------- D.1 the codec

static void test_codec_vectors(void)
{
    static const struct { uint8_t b[13]; adsp_hdr_t h; } V[] = {
        { { 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0x81 }, { 0x1234, 0, 0, 4096, 0x81 } },
        { { 0x56, 0x78, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x00, 0x82 }, { 0x5678, 0, 0, 8192, 0x82 } },
        { { 0xAB, 0xCD, 0, 1, 0, 0, 0, 2, 0, 0, 0x20, 0x00, 0x80 },
          { 0xABCD, 0x00010000, 0x00020000, 8192, 0x80 } },
        { { 0x00, 0x42, 0, 0, 0x03, 0xE8, 0, 0, 0x07, 0xD0, 0x08, 0x00, 0x88 },
          { 0x0042, 1000, 2000, 2048, 0x88 } },
        { { 0x00, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0, 0x06, 0xB4, 0x40 }, { 0x000A, 0, 0, 1716, 0x40 } },
        { { 0x00, 0x0A, 0, 0, 0, 0, 0, 0, 0, 1, 0x00, 0x00, 0x90 }, { 0x000A, 0, 1, 0, 0x90 } },
        { { 0x99, 0x99, 0x00, 0x12, 0xD6, 0x87, 0x00, 0x74, 0xCB, 0xB1, 0x00, 0x00, 0x85 },
          { 0x9999, 1234567, 7654321, 0, 0x85 } },
    };
    for (size_t i = 0; i < sizeof V / sizeof V[0]; i++) {
        adsp_hdr_t h;
        uint8_t out[ADSP_HDR];
        CHECK_EQ_INT(adsp_hdr_parse(V[i].b, ADSP_HDR, &h), 0);
        CHECK_EQ_INT(h.id, V[i].h.id);
        CHECK_EQ_INT(h.first, V[i].h.first);
        CHECK_EQ_INT(h.next, V[i].h.next);
        CHECK_EQ_INT(h.win, V[i].h.win);
        CHECK_EQ_INT(h.desc, V[i].h.desc);
        adsp_hdr_build(out, &V[i].h);
        CHECK_EQ_MEM(out, V[i].b, ADSP_HDR);
    }
    adsp_hdr_t h;
    uint8_t bad[13] = { 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0x99 };
    CHECK_EQ_INT(adsp_hdr_parse(bad, 13, &h), -1);          // an unknown control code
    bad[12] = 0x89;
    CHECK_EQ_INT(adsp_hdr_parse(bad, 13, &h), -1);
    bad[12] = 0x8F;
    CHECK_EQ_INT(adsp_hdr_parse(bad, 13, &h), -1);
    bad[12] = 0x88;
    CHECK_EQ_INT(adsp_hdr_parse(bad, 13, &h), 0);
    bad[12] = 0x0F;                                         // data: the low bits mean nothing
    CHECK_EQ_INT(adsp_hdr_parse(bad, 13, &h), 0);
    CHECK_EQ_INT(adsp_hdr_parse(bad, 3, &h), -1);           // short
    CHECK_EQ_INT(adsp_hdr_parse(bad, 12, &h), -1);
}

// ------------------------------------------------------------- D.3 open

static void test_mac_opens_to_us(void)
{
    fresh(0xA000);
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT_OPEN(0, &MAC, 0xA001, 0x1000, 0x83, 0x4D01);
    CHECK_EQ_INT(n_accept, 1);
    CHECK_EQ_INT(n_opened, 1);
    int c = last_opened;
    CHECK(adsp_is_open(E, c));
    CHECK_EQ_INT(adsp_sock(E, c), US);
    CHECK(adsp_peer(E, c) && adsp_peer(E, c)->addr.node == 20 && adsp_peer(E, c)->addr.sock == 0xE0);
    CHECK(adsp_tx_idle(E, c));
    CHECK_EQ_INT(adsp_tx_free(E, c), ADSP_TX_CAP);

    // The third leg: nothing comes back (it would loop).
    rx_open(&MAC, 0x4D01, 0x0800, 0x82, 0xA001);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(n_opened, 1);
}

static void test_repeated_open_request_gets_the_same_answer(void)
{
    fresh(0xA000);
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    T += 1000;
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    CHECK_EQ_INT(nsent, 2);
    EXPECT_OPEN(1, &MAC, 0xA001, 0x1000, 0x83, 0x4D01);     // the same ConnID
    CHECK_EQ_INT(n_opened, 1);                              // and no second connection
    CHECK_EQ_INT(n_accept, 1);
    // Three places are still free.
    for (int i = 0; i < 3; i++) {
        lt_route_t m = { { 0, (uint8_t)(30 + i), 0xE0 }, (uint8_t)(30 + i), 0 };
        rx_open(&m, 0x4D01, 0x0800, 0x81, 0);
    }
    CHECK_EQ_INT(n_opened, 4);
}

static void test_open_works_without_the_third_leg(void)
{
    fresh(0xA000);
    answer_attn = 1;
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    int c = last_opened;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);                  // no 0x82 ever came
    CHECK_EQ_INT(n_attn, 1);
    CHECK_EQ_INT(attn_conn, c);
    CHECK_EQ_INT(nsent, 3);
    EXPECT(1, &MAC, 0xA001, 0, 1, 0, 0x90, NULL, 0);
    EXPECT(2, &MAC, 0xA001, 0, 0, 0x1000, 0x40, "\0\0", 2);
}

static void test_two_macs_with_one_conn_id_are_two_connections(void)
{
    fresh(0xA000);
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    int c1 = last_opened;
    rx_open(&MAC2, 0x4D01, 0x0800, 0x81, 0);
    int c2 = last_opened;
    CHECK_EQ_INT(n_opened, 2);
    CHECK(c1 != c2);
    EXPECT_OPEN(1, &MAC2, 0xA002, 0x1000, 0x83, 0x4D01);
    CHECK(adsp_peer(E, c2) && adsp_peer(E, c2)->addr.node == 21);

    // Each Mac's bytes reach its own connection.
    rx(&MAC2, 0x4D01, 0, 0, 0x0800, 0x00, "b", 1);
    CHECK_EQ_INT(data_conn, c2);
    rx(&MAC, 0x4D01, 0, 0, 0x0800, 0x00, "a", 1);
    CHECK_EQ_INT(data_conn, c1);
    CHECK_EQ_INT(got_n, 2);
    // The same Mac and ConnID from another of its sockets is a third.
    rx_open(&MACD, 0x4D01, 0x0800, 0x81, 0);
    CHECK_EQ_INT(n_opened, 3);
    // One closing leaves the other.
    rx(&MAC, 0x4D01, 1, 0, 0, 0x85, NULL, 0);
    CHECK(!adsp_is_open(E, c1));
    CHECK(adsp_is_open(E, c2));
}

static void test_open_request_nobody_wants_gets_silence(void)
{
    // The owner says no.
    fresh(0xA000);
    accept_ok = 0;
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    CHECK_EQ_INT(n_accept, 1);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(n_opened, 0);

    // The table is full.
    fresh(0xA000);
    for (int i = 0; i < ADSP_CONNS; i++) {
        lt_route_t m = { { 0, (uint8_t)(30 + i), 0xE0 }, (uint8_t)(30 + i), 0 };
        rx_open(&m, 0x4D01, 0x0800, 0x81, 0);
    }
    CHECK_EQ_INT(n_opened, ADSP_CONNS);
    CHECK_EQ_INT(nsent, ADSP_CONNS);
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    CHECK_EQ_INT(nsent, ADSP_CONNS);
    CHECK_EQ_INT(n_opened, ADSP_CONNS);
    CHECK_EQ_INT(n_accept, ADSP_CONNS);

    // Open packets without their 8 parameter bytes.
    fresh(0xA000);
    uint8_t par[7] = { 0x01, 0x00, 0, 0, 0, 0, 0 };
    rx(&MAC, 0x4D01, 0, 0, 0x0800, 0x81, par, 7);
    rx(&MAC, 0x4D01, 0, 0, 0x0800, 0x81, NULL, 0);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(n_accept, 0);
}

static void test_we_open_to_the_mac(void)
{
    fresh(0xA002);
    int c = adsp_open(E, US, &MACD, T);
    CHECK(c >= 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT_OPEN(0, &MACD, 0xA003, 0x1000, 0x81, 0);
    CHECK(!adsp_is_open(E, c));
    CHECK_EQ_INT(adsp_sock(E, c), US);
    CHECK_EQ_INT(adsp_tx_free(E, c), 0);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"x", 1, T), 0);
    CHECK_EQ_INT(n_opened, 0);

    T += 300;
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(nsent, 2);
    EXPECT_OPEN(1, &MACD, 0xA003, 0x1000, 0x82, 0x4D02);
    CHECK_EQ_INT(n_opened, 1);
    CHECK_EQ_INT(last_opened, c);
    CHECK(adsp_is_open(E, c));
    // No more Open Requests.
    for (int i = 0; i < 40; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(count_desc(0, 0x81), 1);

    // A Request+Ack that names another ConnID is not ours.
    fresh(0xA002);
    c = adsp_open(E, US, &MACD, T);
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA004);
    CHECK_EQ_INT(nsent, 1);
    CHECK(!adsp_is_open(E, c));
}

static void test_open_to_a_silent_mac_gives_up(void)
{
    fresh(0xA002);
    uint64_t t0 = T;
    int c = adsp_open(E, US, &MACD, T);
    uint64_t at[8] = { 0 };
    int seen = 1;
    uint64_t gave_up = 0;
    for (int i = 0; i < 100; i++) {
        T += 100;
        adsp_tick(E, T);
        if (nsent > seen && seen < 8) at[seen++] = T - t0;
        if (n_closed && !gave_up) gave_up = T - t0;
    }
    CHECK_EQ_INT(nsent, 1 + ADSP_OPEN_RETRIES);             // again each second, five times
    for (int i = 0; i < nsent; i++) {
        EXPECT_OPEN(i, &MACD, 0xA003, 0x1000, 0x81, 0);
        CHECK_EQ_INT(at[i], (uint64_t)i * 1000);
    }
    CHECK_EQ_INT(n_closed, 1);
    CHECK_EQ_INT(last_closed, c);
    CHECK_EQ_INT(last_why, ADSP_END_OPEN);
    CHECK_EQ_INT(gave_up, 6000);
    CHECK(!adsp_is_open(E, c));
    CHECK_EQ_INT(adsp_sock(E, c), 0);                       // idle again
    CHECK(adsp_peer(E, c) == NULL);
    // A late answer opens nothing.
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(n_opened, 0);
    CHECK_EQ_INT(nsent, 1 + ADSP_OPEN_RETRIES);
}

static void test_open_denied_gives_up_at_once(void)
{
    fresh(0xA002);
    int c = adsp_open(E, US, &MACD, T);
    rx_open(&MACD, 0x4D02, 0x0800, 0x84, 0xA009);           // someone else's
    CHECK_EQ_INT(n_closed, 0);
    rx_open(&MACD, 0x4D02, 0x0800, 0x84, 0xA003);
    CHECK_EQ_INT(n_closed, 1);
    CHECK_EQ_INT(last_closed, c);
    CHECK_EQ_INT(last_why, ADSP_END_OPEN);
    for (int i = 0; i < 80; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(n_closed, 1);
}

static void test_second_request_ack_from_a_new_conn_id_is_adopted(void)
{
    // While the open is still pending.
    fresh(0xA002);
    int c = adsp_open(E, US, &MACD, T);
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA003);
    rx_open(&MACD, 0x4D03, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(nsent, 3);
    EXPECT_OPEN(1, &MACD, 0xA003, 0x1000, 0x82, 0x4D02);
    EXPECT_OPEN(2, &MACD, 0xA003, 0x1000, 0x82, 0x4D03);
    CHECK_EQ_INT(n_opened, 1);                              // one connection, told once
    CHECK(adsp_is_open(E, c));
    // The newer one's data is taken, the older one's is not.
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, "old", 3);
    CHECK_EQ_INT(got_n, 0);
    CHECK_EQ_INT(nsent, 3);
    rx(&MACD, 0x4D03, 0, 0, 0x0800, 0x40, "new", 3);
    CHECK_EQ_INT(got_n, 3);
    CHECK_EQ_MEM(got, "new", 3);
    CHECK_EQ_INT(data_conn, c);
    EXPECT(3, &MACD, 0xA003, 0, 3, 0x1000, 0x80, NULL, 0);
    // Once bytes have come, a third ConnID is nobody.
    rx_open(&MACD, 0x4D04, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(nsent, 4);
    rx(&MACD, 0x4D03, 3, 0, 0x0800, 0x00, "!", 1);
    CHECK_EQ_INT(got_n, 4);

    // Bytes written before the newer one arrived go to it.
    fresh(0xA002);
    c = adsp_open(E, US, &MACD, T);
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"hi", 2, T), 2);
    rx_open(&MACD, 0x4D03, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(nsent, 5);
    EXPECT(2, &MACD, 0xA003, 0, 0, 0x1000, 0x40, "hi", 2);
    EXPECT_OPEN(3, &MACD, 0xA003, 0x1000, 0x82, 0x4D03);
    EXPECT(4, &MACD, 0xA003, 0, 0, 0x1000, 0x40, "hi", 2);
    rx(&MACD, 0x4D03, 0, 2, 0x0800, 0x80, NULL, 0);
    CHECK(adsp_tx_idle(E, c));
}

static void test_repeated_request_ack_gets_the_ack_again(void)
{
    fresh(0xA002);
    adsp_open(E, US, &MACD, T);
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA003);
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA003);
    CHECK_EQ_INT(nsent, 3);
    EXPECT_OPEN(2, &MACD, 0xA003, 0x1000, 0x82, 0x4D02);
    CHECK_EQ_INT(n_opened, 1);
}

static void test_conn_ids(void)
{
    // Never 0, never one in use.
    fresh(0xFFFE);
    adsp_open(E, US, &MACD, T);
    adsp_open(E, US, &MAC, T);
    adsp_open(E, US, &MAC2, T);
    CHECK_EQ_INT(nsent, 3);
    EXPECT_OPEN(0, &MACD, 0xFFFF, 0x1000, 0x81, 0);
    EXPECT_OPEN(1, &MAC, 0x0001, 0x1000, 0x81, 0);
    EXPECT_OPEN(2, &MAC2, 0x0002, 0x1000, 0x81, 0);
    int c = adsp_open(E, US, &MACD, T);
    CHECK(c >= 0);
    CHECK_EQ_INT(adsp_open(E, US, &MACD, T), -1);           // the table is full
    CHECK_EQ_INT(nsent, 4);

    // The counter comes round to one still in use and steps over it.
    fresh(0x0000);
    int keep = adsp_open(E, US, &MAC, T);                   // 0001
    for (long i = 0; i < 0xFFFE; i++) {
        int k = adsp_open(E, US, &MACD, T);
        CHECK(k >= 0);
        nsent = 0;
        adsp_close(E, k, T);
    }
    nsent = 0;
    adsp_open(E, US, &MACD, T);                             // 0001 is taken, and 0 is no ID
    EXPECT_OPEN(0, &MACD, 0x0002, 0x1000, 0x81, 0);
    CHECK_EQ_INT(adsp_sock(E, keep), US);
}

// ------------------------------------------------------------- D.4 attention

static void test_attention(void)
{
    int c = mac_opens(0x0800);
    answer_attn = 1;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);
    CHECK_EQ_INT(n_attn, 1);
    CHECK_EQ_INT(attn_conn, c);
    CHECK_EQ_INT(attn_code, 0x0011);
    CHECK_EQ_INT(attn_n, 1);
    CHECK_EQ_INT(attn_body[0], 0x00);
    CHECK_EQ_INT(nsent, 2);
    EXPECT(0, &MAC, 0xA001, 0, 1, 0, 0x90, NULL, 0);        // the ack first, 13 bytes
    EXPECT(1, &MAC, 0xA001, 0, 0, 0x1000, 0x40, "\0\0", 2); // then the answer, no EOM
    rx(&MAC, 0x4D01, 0, 2, 0x0800, 0x80, NULL, 0);
    CHECK(adsp_tx_idle(E, c));
    CHECK_EQ_INT(nsent, 2);

    // An unknown code is delivered like any other (the owner answers 00 00).
    uint8_t u[3] = { 0x00, 0x42, 0x00 };
    rx(&MAC, 0x4D01, 1, 0, 0, 0x50, u, 3);
    CHECK_EQ_INT(n_attn, 2);
    CHECK_EQ_INT(attn_code, 0x0042);
    CHECK_EQ_INT(nsent, 4);
    EXPECT(2, &MAC, 0xA001, 0, 2, 0, 0x90, NULL, 0);
    EXPECT(3, &MAC, 0xA001, 2, 0, 0x1000, 0x40, "\0\0", 2);

    // A code and no body; a full 570-byte body.
    uint8_t big[2 + ADSP_ATTN_MAX + 1] = { 0x00, 0x0A };
    for (int i = 2; i < (int)sizeof big; i++) big[i] = (uint8_t)i;
    rx(&MAC, 0x4D01, 2, 0, 0, 0x50, big, 2);
    CHECK_EQ_INT(n_attn, 3);
    CHECK_EQ_INT(attn_n, 0);
    rx(&MAC, 0x4D01, 3, 0, 0, 0x50, big, 2 + ADSP_ATTN_MAX);
    CHECK_EQ_INT(n_attn, 4);
    CHECK_EQ_INT(attn_n, ADSP_ATTN_MAX);
    CHECK_EQ_MEM(attn_body, big + 2, ADSP_ATTN_MAX);
    // A body over 570, or no code at all: dropped, not acked.
    int before = nsent;
    rx(&MAC, 0x4D01, 4, 0, 0, 0x50, big, 2 + ADSP_ATTN_MAX + 1);
    rx(&MAC, 0x4D01, 4, 0, 0, 0x50, big, 1);
    rx(&MAC, 0x4D01, 4, 0, 0, 0x50, NULL, 0);
    CHECK_EQ_INT(n_attn, 4);
    CHECK_EQ_INT(nsent, before);
    rx(&MAC, 0x4D01, 4, 0, 0, 0x50, q, 3);                  // and the sequence did not move
    CHECK_EQ_INT(n_attn, 5);
}

static void test_repeated_attention_is_acked_not_delivered(void)
{
    mac_opens(0x0800);
    answer_attn = 1;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);
    rx(&MAC, 0x4D01, 0, 2, 0x0800, 0x80, NULL, 0);
    CHECK_EQ_INT(nsent, 2);
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);                  // its ack was lost
    CHECK_EQ_INT(nsent, 3);
    EXPECT(2, &MAC, 0xA001, 0, 1, 0, 0x90, NULL, 0);        // acked again
    CHECK_EQ_INT(n_attn, 1);                                // and no second answer
    rx(&MAC, 0x4D01, 0, 0, 0, 0x10, q, 3);                  // nor when it does not ask
    CHECK_EQ_INT(nsent, 4);
    EXPECT(3, &MAC, 0xA001, 0, 1, 0, 0x90, NULL, 0);
    CHECK_EQ_INT(n_attn, 1);
}

static void test_attention_without_ack_request_is_acked(void)
{
    mac_opens(0x0800);                                      // lpstyl sends 0x10
    answer_attn = 1;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MAC, 0x4D01, 0, 0, 0, 0x10, q, 3);
    CHECK_EQ_INT(n_attn, 1);
    CHECK_EQ_INT(nsent, 2);
    EXPECT(0, &MAC, 0xA001, 0, 1, 0, 0x90, NULL, 0);
    EXPECT(1, &MAC, 0xA001, 0, 0, 0x1000, 0x40, "\0\0", 2);
}

static void test_attention_out_of_sequence(void)
{
    mac_opens(0x0800);
    answer_attn = 1;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MAC, 0x4D01, 5, 0, 0, 0x50, q, 3);                  // 0 is expected
    CHECK_EQ_INT(n_attn, 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MAC, 0xA001, 0, 0, 0, 0x90, NULL, 0);        // acked with next 0
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);
    CHECK_EQ_INT(n_attn, 1);
}

// Review focus 2.
static void test_attention_leaves_the_stream_alone(void)
{
    int c = mac_opens(0x0800);
    answer_attn = 1;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);
    CHECK_EQ_INT(nsent, 2);
    CHECK(!adsp_tx_idle(E, c));                             // the answer is unacknowledged

    // The next attention's window 0 and next 0 are its own, not the stream's:
    // read as a stream ack they would shut the Mac's window and hold this
    // answer back for good.
    rx(&MAC, 0x4D01, 1, 0, 0, 0x50, q, 3);
    CHECK_EQ_INT(nsent, 4);
    EXPECT(2, &MAC, 0xA001, 0, 2, 0, 0x90, NULL, 0);
    EXPECT(3, &MAC, 0xA001, 2, 0, 0x1000, 0x40, "\0\0", 2);

    // Nor do an attention's sequence fields acknowledge stream bytes.
    answer_attn = 0;
    rx(&MAC, 0x4D01, 2, 4, 0, 0x50, q, 3);
    CHECK_EQ_INT(n_attn, 3);
    CHECK(!adsp_tx_idle(E, c));
    CHECK_EQ_INT(adsp_tx_free(E, c), ADSP_TX_CAP - 4);

    // An attention ack for nothing we sent: ignored, whatever it claims.
    int before = nsent;
    rx(&MAC, 0x4D01, 0, 4, 0, 0x90, NULL, 0);
    rx(&MAC, 0x4D01, 0, 4, 0, 0x90, "\0\x11", 2);           // 15 bytes, as lpstyl's
    CHECK_EQ_INT(nsent, before);
    CHECK(!adsp_tx_idle(E, c));
    CHECK_EQ_INT(n_attn, 3);

    // The stream's receive sequence did not move with the attentions': byte 0
    // is still the one expected, and what we ack names the stream.
    rx(&MAC, 0x4D01, 0, 4, 0x0800, 0x40, "ab", 2);
    CHECK_EQ_INT(got_n, 2);
    CHECK(adsp_tx_idle(E, c));                              // a stream packet's ack does count
    CHECK_EQ_INT(nsent, before + 1);
    EXPECT(before, &MAC, 0xA001, 4, 2, 0x1000, 0x80, NULL, 0);

    // And the adapter still sends.
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"ok", 2, T), 2);
    EXPECT(before + 1, &MAC, 0xA001, 4, 2, 0x1000, 0x40, "ok", 2);

    // An attention on a connection whose stream window is shut is still
    // acked, and the window stays shut.
    c = mac_opens(0);
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MAC, 0xA001, 0, 1, 0, 0x90, NULL, 0);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"zz", 2, T), 2);
    CHECK_EQ_INT(nsent, 1);
}

// ------------------------------------------------------------- D.5 data

static void test_data_both_ways(void)
{
    int c = we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x60, "?", 1);          // '?' as a message: M 0 -> 2
    CHECK_EQ_INT(got_n, 1);
    CHECK_EQ_INT(got[0], '?');
    CHECK_EQ_INT(n_eom, 1);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MACD, 0xA003, 0, 2, 0x1000, 0x80, NULL, 0);  // the ack names 2, not 1

    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"CS\r", 3, T), 3);
    CHECK_EQ_INT(nsent, 2);
    EXPECT(1, &MACD, 0xA003, 0, 2, 0x1000, 0x40, "CS\r", 3);   // no EOM: A 0 -> 3
    CHECK(!adsp_tx_idle(E, c));
    rx(&MACD, 0x4D02, 2, 3, 0x0800, 0x80, NULL, 0);
    CHECK(adsp_tx_idle(E, c));
    CHECK_EQ_INT(nsent, 2);

    rx(&MACD, 0x4D02, 2, 3, 0x0800, 0x60, "\xFF\xFF\xFF\x31", 4);   // M 2 -> 7
    CHECK_EQ_INT(got_n, 5);
    CHECK_EQ_INT(n_eom, 2);
    EXPECT(2, &MACD, 0xA003, 3, 7, 0x1000, 0x80, NULL, 0);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"\x08", 1, T), 1);
    CHECK_EQ_INT(nsent, 4);
    EXPECT(3, &MACD, 0xA003, 3, 7, 0x1000, 0x40, "\x08", 1);        // A 3 -> 4
    CHECK_EQ_MEM(got, "?\xFF\xFF\xFF\x31", 5);
}

static void test_message_across_two_packets(void)
{
    we_open(0x0800);
    uint8_t m[600];
    for (int i = 0; i < 600; i++) m[i] = (uint8_t)(i * 7 + 1);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, m, 572);
    CHECK_EQ_INT(n_eom, 0);
    EXPECT(0, &MACD, 0xA003, 0, 572, 0x1000, 0x80, NULL, 0);
    rx(&MACD, 0x4D02, 572, 0, 0x0800, 0x60, m + 572, 28);
    CHECK_EQ_INT(n_eom, 1);
    EXPECT(1, &MACD, 0xA003, 0, 601, 0x1000, 0x80, NULL, 0);    // 601 more than the start
    CHECK_EQ_INT(got_n, 600);
    CHECK_EQ_MEM(got, m, 600);
}

static void test_bare_eom(void)
{
    we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, "ab", 2);
    CHECK_EQ_INT(n_data, 1);
    rx(&MACD, 0x4D02, 2, 0, 0x0800, 0x60, NULL, 0);
    CHECK_EQ_INT(n_data, 2);                                // told, with no bytes
    CHECK_EQ_INT(n_eom, 1);
    CHECK_EQ_INT(got_n, 2);
    EXPECT(1, &MACD, 0xA003, 0, 3, 0x1000, 0x80, NULL, 0);  // it took one sequence number
    rx(&MACD, 0x4D02, 2, 0, 0x0800, 0x60, NULL, 0);         // again: acked, not told twice
    CHECK_EQ_INT(n_eom, 1);
    CHECK_EQ_INT(n_data, 2);
    EXPECT(2, &MACD, 0xA003, 0, 3, 0x1000, 0x80, NULL, 0);
    rx(&MACD, 0x4D02, 3, 0, 0x0800, 0x40, "c", 1);
    CHECK_EQ_INT(got_n, 3);
    EXPECT(3, &MACD, 0xA003, 0, 4, 0x1000, 0x80, NULL, 0);

    // A bare EOM needs no room.
    we_open(0x0800);
    room_val = 0;
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x60, NULL, 0);
    CHECK_EQ_INT(n_eom, 1);
    EXPECT(0, &MACD, 0xA003, 0, 1, 0, 0x80, NULL, 0);
}

static void test_data_without_ack_request_is_not_acked(void)
{
    we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x00, "ab", 2);
    rx(&MACD, 0x4D02, 2, 0, 0x0800, 0x20, "cd", 2);
    CHECK_EQ_INT(got_n, 4);
    CHECK_EQ_INT(n_eom, 1);
    CHECK_EQ_INT(nsent, 0);
    rx(&MACD, 0x4D02, 5, 0, 0x0800, 0x40, NULL, 0);         // asks, carries nothing
    EXPECT(0, &MACD, 0xA003, 0, 5, 0x1000, 0x80, NULL, 0);
}

static void test_duplicate_ahead_and_overlap(void)
{
    we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, "abcd", 4);
    EXPECT(0, &MACD, 0xA003, 0, 4, 0x1000, 0x80, NULL, 0);

    // A duplicate: not delivered twice, acked again.
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, "abcd", 4);
    CHECK_EQ_INT(got_n, 4);
    CHECK_EQ_INT(nsent, 2);
    EXPECT(1, &MACD, 0xA003, 0, 4, 0x1000, 0x80, NULL, 0);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x00, "abcd", 4);       // and one that does not ask
    CHECK_EQ_INT(got_n, 4);
    CHECK_EQ_INT(nsent, 2);

    // Ahead of sequence: dropped; the ack still names the old sequence.
    rx(&MACD, 0x4D02, 6, 0, 0x0800, 0x40, "gh", 2);
    CHECK_EQ_INT(got_n, 4);
    CHECK_EQ_INT(nsent, 3);
    EXPECT(2, &MACD, 0xA003, 0, 4, 0x1000, 0x80, NULL, 0);

    // Overlapping by 2 bytes: only the new bytes are delivered.
    rx(&MACD, 0x4D02, 2, 0, 0x0800, 0x40, "cdef", 4);
    CHECK_EQ_INT(got_n, 6);
    EXPECT(3, &MACD, 0xA003, 0, 6, 0x1000, 0x80, NULL, 0);
    rx(&MACD, 0x4D02, 6, 0, 0x0800, 0x60, "gh", 2);
    CHECK_EQ_INT(got_n, 8);
    CHECK_EQ_MEM(got, "abcdefgh", 8);
    CHECK_EQ_INT(n_eom, 1);
    EXPECT(4, &MACD, 0xA003, 0, 9, 0x1000, 0x80, NULL, 0);

    // A message that ended, sent again: a duplicate, its EOM not counted twice.
    rx(&MACD, 0x4D02, 6, 0, 0x0800, 0x60, "gh", 2);
    CHECK_EQ_INT(got_n, 8);
    CHECK_EQ_INT(n_eom, 1);
    EXPECT(5, &MACD, 0xA003, 0, 9, 0x1000, 0x80, NULL, 0);

    // Bytes that came without their EOM, sent again with it: the EOM is new.
    rx(&MACD, 0x4D02, 9, 0, 0x0800, 0x00, "ij", 2);
    rx(&MACD, 0x4D02, 9, 0, 0x0800, 0x60, "ij", 2);
    CHECK_EQ_INT(got_n, 10);
    CHECK_EQ_INT(n_eom, 2);
    EXPECT(6, &MACD, 0xA003, 0, 12, 0x1000, 0x80, NULL, 0);
}

static void test_bytes_in_order_across_packets_and_messages(void)
{
    we_open(0x0800);
    room_val = 0xFFFF;
    static uint8_t in[9000];
    for (size_t i = 0; i < sizeof in; i++) in[i] = (uint8_t)(i * 31 + (i >> 8));
    uint32_t seq = 0;
    size_t pos = 0;
    int msgs = 0;
    for (int k = 0; pos < sizeof in; k++) {
        size_t n = (size_t)(k * 97 % 572) + 1;
        if (n > sizeof in - pos) n = sizeof in - pos;
        int eom = k % 3 == 0;
        rx(&MACD, 0x4D02, seq, 0, 0x0800, (uint8_t)(eom ? 0x60 : 0x40), in + pos, n);
        if (k % 5 == 0)                                     // a repeat here and there
            rx(&MACD, 0x4D02, seq, 0, 0x0800, (uint8_t)(eom ? 0x60 : 0x40), in + pos, n);
        seq += (uint32_t)n + (eom ? 1 : 0);
        pos += n;
        msgs += eom;
    }
    CHECK_EQ_INT(got_n, sizeof in);
    CHECK_EQ_MEM(got, in, sizeof in);
    CHECK_EQ_INT(n_eom, msgs);
}

static void test_write_with_no_connection_sends_nothing(void)
{
    fresh(0xA000);
    CHECK_EQ_INT(adsp_write(E, 0, (const uint8_t *)"B", 1, T), 0);
    CHECK_EQ_INT(adsp_write(E, -1, (const uint8_t *)"B", 1, T), 0);
    CHECK_EQ_INT(adsp_write(E, ADSP_CONNS, (const uint8_t *)"B", 1, T), 0);
    CHECK_EQ_INT(adsp_tx_free(E, 0), 0);
    CHECK_EQ_INT(adsp_tx_free(E, 99), 0);
    CHECK(adsp_tx_idle(E, 0));
    CHECK(!adsp_is_open(E, 0) && !adsp_is_open(E, -1) && !adsp_is_open(E, 99));
    CHECK_EQ_INT(adsp_sock(E, 0), 0);
    CHECK(adsp_peer(E, 0) == NULL && adsp_peer(E, -1) == NULL);
    adsp_window(E, 0, T);
    adsp_close(E, 0, T);
    adsp_close(E, 99, T);
    adsp_tick(E, T + 60000);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(n_closed, 0);

    // Our bytes go out 572 to a packet, and no more than ADSP_TX_CAP are kept.
    int c = we_open(0x0800);
    uint8_t b[700];
    for (int i = 0; i < 700; i++) b[i] = (uint8_t)(i + 3);
    CHECK_EQ_INT(adsp_write(E, c, b, 700, T), ADSP_TX_CAP);
    CHECK_EQ_INT(adsp_tx_free(E, c), 0);
    CHECK_EQ_INT(nsent, 2);
    EXPECT(0, &MACD, 0xA003, 0, 0, 0x1000, 0x40, b, 572);
    EXPECT(1, &MACD, 0xA003, 572, 0, 0x1000, 0x40, b + 572, ADSP_TX_CAP - 572);
    rx(&MACD, 0x4D02, 0, 572, 0x0800, 0x80, NULL, 0);
    CHECK_EQ_INT(adsp_tx_free(E, c), 572);
    CHECK_EQ_INT(adsp_write(E, c, b + ADSP_TX_CAP, 60, T), 60);
    EXPECT(2, &MACD, 0xA003, ADSP_TX_CAP, 0, 0x1000, 0x40, b + ADSP_TX_CAP, 60);
    rx(&MACD, 0x4D02, 0, 700, 0x0800, 0x80, NULL, 0);
    CHECK(adsp_tx_idle(E, c));
    // An ack for more than was sent, or for less than is acknowledged: nothing.
    adsp_write(E, c, b, 10, T);
    rx(&MACD, 0x4D02, 0, 711, 0x0800, 0x80, NULL, 0);
    rx(&MACD, 0x4D02, 0, 600, 0x0800, 0x80, NULL, 0);
    CHECK(!adsp_tx_idle(E, c));
    CHECK_EQ_INT(adsp_tx_free(E, c), ADSP_TX_CAP - 10);
    rx(&MACD, 0x4D02, 0, 704, 0x0800, 0x80, NULL, 0);       // part of it
    CHECK_EQ_INT(adsp_tx_free(E, c), ADSP_TX_CAP - 6);
}

// ------------------------------------------------------------- D.6 window

static void test_bytes_beyond_room_are_not_taken(void)
{
    we_open(0x0800);
    room_val = 40;
    uint8_t m[100];
    for (int i = 0; i < 100; i++) m[i] = (uint8_t)(i + 1);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x60, m, 100);
    CHECK_EQ_INT(got_n, 40);
    CHECK_EQ_INT(n_eom, 0);                                 // the message has not ended
    room_val = 0;                                           // the taker is full
    EXPECT(0, &MACD, 0xA003, 0, 40, 40, 0x80, NULL, 0);     // (asked before the bytes landed)
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x60, m, 100);          // again, nothing fits
    CHECK_EQ_INT(got_n, 40);
    EXPECT(1, &MACD, 0xA003, 0, 40, 0, 0x80, NULL, 0);      // the ack names what was taken
    room_val = 0x1000;
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x60, m, 100);          // sent again once there is room
    CHECK_EQ_INT(got_n, 100);
    CHECK_EQ_MEM(got, m, 100);
    CHECK_EQ_INT(n_eom, 1);
    EXPECT(2, &MACD, 0xA003, 0, 101, 0x1000, 0x80, NULL, 0);
}

// The Mac's side of a transfer that obeys our window: what it may send up to.
static uint32_t mac_limit(int *seen, uint32_t limit, int *zero)
{
    for (; *seen < nsent; (*seen)++) {
        adsp_hdr_t h;
        CHECK_EQ_INT(adsp_hdr_parse(sent[*seen].p, sent[*seen].n, &h), 0);
        if (h.desc & ADSP_ATTENTION) continue;
        limit = h.next + h.win;
        if (!h.win) *zero = 1;
    }
    return limit;
}

// Review focus 5: 20 KB through a 4096-byte room that drains in steps.
static void test_window_shut_then_opened_is_announced(void)
{
    fresh(0xA000);
    sink_cap = 4096;
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    int c = last_opened;
    EXPECT_OPEN(0, &MAC, 0xA001, 4096, 0x83, 0x4D01);

    static uint8_t in[20 * 1024];
    for (size_t i = 0; i < sizeof in; i++) in[i] = (uint8_t)(i * 13 + (i >> 7));
    uint32_t seq = 0, limit = 0;
    size_t pos = 0, peak = 0;
    int seen = 0, zero = 0, announced = 0, guard = 0;
    while (pos < sizeof in && guard++ < 100000) {
        limit = mac_limit(&seen, limit, &zero);
        size_t can = limit - seq;
        if (can > ADSP_MAX_DATA) can = ADSP_MAX_DATA;
        if (can > sizeof in - pos) can = sizeof in - pos;
        if (can) {                                          // the Mac sends what the window allows
            rx(&MAC, 0x4D01, seq, 0, 0x0800, 0x40, in + pos, can);
            seq += (uint32_t)can;
            pos += can;
            if (held > peak) peak = held;
            continue;
        }
        // The window is shut and the Mac sends nothing, not even a probe. The
        // printer takes a step; the news has to go out by itself.
        CHECK_EQ_INT(held, sink_cap);
        size_t step = (size_t)(guard * 131 % 900) + 100;
        held -= step;
        int before = nsent;
        T += 50;
        adsp_window(E, c, T);
        CHECK_EQ_INT(nsent, before + 1);
        EXPECT(before, &MAC, 0xA001, 0, seq, (uint16_t)step, 0x80, NULL, 0);
        adsp_window(E, c, T);                               // told once
        CHECK_EQ_INT(nsent, before + 1);
        announced++;
    }
    CHECK_EQ_INT(pos, sizeof in);
    CHECK_EQ_INT(got_n, sizeof in);                         // nothing lost
    CHECK_EQ_MEM(got, in, sizeof in);                       // or reordered
    CHECK_EQ_INT(peak, sink_cap);                           // the room filled, and no further
    CHECK(zero);                                            // the window did fall to 0
    CHECK(announced >= 4);
}

static void test_window_filled_without_an_ack_is_still_announced(void)
{
    // The Mac fills the window with packets that ask for no ack, so it was
    // never told 0; by its own count it is shut all the same.
    fresh(0xA000);
    sink_cap = 1000;
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    int c = last_opened;
    static uint8_t b[1000];
    rx(&MAC, 0x4D01, 0, 0, 0x0800, 0x00, b, 500);
    rx(&MAC, 0x4D01, 500, 0, 0x0800, 0x00, b, 500);
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(held, 1000);
    adsp_window(E, c, T);                                   // still full: nothing to say
    CHECK_EQ_INT(nsent, 1);
    held = 700;
    adsp_window(E, c, T);
    CHECK_EQ_INT(nsent, 2);
    EXPECT(1, &MAC, 0xA001, 0, 1000, 300, 0x80, NULL, 0);

    // A window that was never shut is not announced.
    fresh(0xA000);
    sink_cap = 1000;
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    c = last_opened;
    rx(&MAC, 0x4D01, 0, 0, 0x0800, 0x40, b, 500);
    CHECK_EQ_INT(nsent, 2);
    held = 0;
    adsp_window(E, c, T);
    CHECK_EQ_INT(nsent, 2);
}

static void test_far_window_zero_holds_our_bytes(void)
{
    int c = we_open(0);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"abc", 3, T), 3);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"de", 2, T), 2);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(adsp_tx_free(E, c), ADSP_TX_CAP - 5);
    CHECK(!adsp_tx_idle(E, c));
    rx(&MACD, 0x4D02, 0, 0, 0, 0x80, NULL, 0);              // still shut
    CHECK_EQ_INT(nsent, 0);
    for (int i = 0; i < 40; i++) { T += 100; adsp_tick(E, T); rx(&MACD, 0x4D02, 0, 0, 0, 0x80, NULL, 0); }
    CHECK_EQ_INT(nsent, 0);                                 // nothing in flight, nothing to resend
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x80, NULL, 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MACD, 0xA003, 0, 0, 0x1000, 0x40, "abcde", 5);   // released, in order
}

static void test_far_window_of_three(void)
{
    int c = we_open(3);
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"abcdefgh", 8, T), 8);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MACD, 0xA003, 0, 0, 0x1000, 0x40, "abc", 3);     // at most 3 before the next ack
    CHECK_EQ_INT(adsp_write(E, c, (const uint8_t *)"ij", 2, T), 2);
    CHECK_EQ_INT(nsent, 1);
    rx(&MACD, 0x4D02, 0, 2, 3, 0x80, NULL, 0);                  // two taken, three more allowed
    CHECK_EQ_INT(nsent, 2);
    EXPECT(1, &MACD, 0xA003, 3, 0, 0x1000, 0x40, "de", 2);
    rx(&MACD, 0x4D02, 0, 5, 3, 0x80, NULL, 0);
    EXPECT(2, &MACD, 0xA003, 5, 0, 0x1000, 0x40, "fgh", 3);
    rx(&MACD, 0x4D02, 0, 8, 3, 0x80, NULL, 0);
    EXPECT(3, &MACD, 0xA003, 8, 0, 0x1000, 0x40, "ij", 2);
    rx(&MACD, 0x4D02, 0, 10, 3, 0x80, NULL, 0);
    CHECK_EQ_INT(nsent, 4);
    CHECK(adsp_tx_idle(E, c));
}

// ------------------------------------------------------------- D.7 resend, probes, close

static void test_lost_data_comes_again(void)
{
    int c = we_open(0x0800);
    uint64_t t0 = T;
    adsp_write(E, c, (const uint8_t *)"CS\r", 3, T);
    CHECK_EQ_INT(nsent, 1);
    uint64_t at[8] = { 0 }, closed_at = 0;
    int n = 0;
    for (int i = 0; i < 400; i++) {
        T += 100;
        int before = nsent;
        adsp_tick(E, T);
        for (int k = before; k < nsent; k++) if (desc_of(k) == 0x40 && n < 8) at[n++] = T - t0;
        if (n_closed && !closed_at) closed_at = T - t0;
        // The Mac is there, answering probes; it just never acks the data.
        if (nsent > before && desc_of(nsent - 1) == 0xC0) rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x80, NULL, 0);
    }
    CHECK_EQ_INT(n, ADSP_RESENDS);                          // 3 s after the last, five times
    for (int i = 0; i < n; i++) CHECK_EQ_INT(at[i], (uint64_t)(i + 1) * 3000);
    for (int k = 0; k < nsent; k++)
        if (desc_of(k) == 0x40) EXPECT(k, &MACD, 0xA003, 0, 0, 0x1000, 0x40, "CS\r", 3);   // the same FirstByteSeq
    CHECK_EQ_INT(n_closed, 1);                              // then the connection is gone
    CHECK_EQ_INT(last_closed, c);
    CHECK_EQ_INT(last_why, ADSP_END_RESEND);
    CHECK_EQ_INT(closed_at, 18000);
    CHECK_EQ_INT(count_desc(0, 0x85), 1);                   // after one Close Advice
    CHECK_EQ_INT(desc_of(nsent - 1), 0x85);
    EXPECT(nsent - 1, &MACD, 0xA003, 3, 0, 0, 0x85, NULL, 0);
    CHECK(!adsp_is_open(E, c));

    // An ack in time stops it, and starts the count again for later bytes.
    c = we_open(0x0800);
    adsp_write(E, c, (const uint8_t *)"CS\r", 3, T);
    for (int i = 0; i < 100; i++) {                         // 10 s: three resends
        T += 100; adsp_tick(E, T);
        if (desc_of(nsent - 1) == 0xC0) rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x80, NULL, 0);
    }
    CHECK_EQ_INT(count_desc(0, 0x40), 4);
    rx(&MACD, 0x4D02, 0, 3, 0x0800, 0x80, NULL, 0);
    CHECK(adsp_tx_idle(E, c));
    nsent = 0;
    adsp_write(E, c, (const uint8_t *)"B", 1, T);
    for (int i = 0; i < 140; i++) {                         // 14 s: four more, and still open
        T += 100; adsp_tick(E, T);
        if (desc_of(nsent - 1) == 0xC0) rx(&MACD, 0x4D02, 0, 3, 0x0800, 0x80, NULL, 0);
    }
    CHECK_EQ_INT(count_desc(0, 0x40), 5);
    CHECK(adsp_is_open(E, c));
    CHECK_EQ_INT(n_closed, 0);

    // A resend is everything unacknowledged, 572 bytes to a packet.
    c = we_open(0x0800);
    uint8_t b[600];
    for (int i = 0; i < 600; i++) b[i] = (uint8_t)(i * 3);
    adsp_write(E, c, b, 600, T);
    CHECK_EQ_INT(nsent, 2);
    T += 2900; adsp_tick(E, T);
    CHECK_EQ_INT(nsent, 2);
    T += 100; adsp_tick(E, T);
    CHECK_EQ_INT(nsent, 4);
    EXPECT(2, &MACD, 0xA003, 0, 0, 0x1000, 0x40, b, 572);
    EXPECT(3, &MACD, 0xA003, 572, 0, 0x1000, 0x40, b + 572, 28);
}

static void test_retransmit_advice(void)
{
    int c = we_open(0x0800);
    adsp_write(E, c, (const uint8_t *)"CS\r", 3, T);
    T += 200;
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x88, NULL, 0);
    CHECK_EQ_INT(nsent, 2);                                 // at once, from sequence 0
    EXPECT(1, &MACD, 0xA003, 0, 0, 0x1000, 0x40, "CS\r", 3);

    // Its ack is applied first: the resend starts from there.
    rx(&MACD, 0x4D02, 0, 1, 0x0800, 0x88, NULL, 0);
    CHECK_EQ_INT(nsent, 3);
    EXPECT(2, &MACD, 0xA003, 1, 0, 0x1000, 0x40, "S\r", 2);
    CHECK_EQ_INT(adsp_tx_free(E, c), ADSP_TX_CAP - 2);
    // With everything acknowledged there is nothing to send.
    rx(&MACD, 0x4D02, 0, 3, 0x0800, 0x88, NULL, 0);
    CHECK_EQ_INT(nsent, 3);
    CHECK(adsp_tx_idle(E, c));
    // The timer counts from the resend.
    adsp_write(E, c, (const uint8_t *)"x", 1, T);
    T += 2000;
    rx(&MACD, 0x4D02, 0, 3, 0x0800, 0x88, NULL, 0);
    CHECK_EQ_INT(nsent, 5);
    T += 2000; adsp_tick(E, T);
    CHECK_EQ_INT(nsent, 5);
    T += 1000; adsp_tick(E, T);
    CHECK_EQ_INT(nsent, 6);
}

static void test_probe_gets_one_ack(void)
{
    we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0xC0, NULL, 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MACD, 0xA003, 0, 0, 0x1000, 0x80, NULL, 0);
    T += 100; adsp_tick(E, T);
    T += 100; adsp_tick(E, T);
    CHECK_EQ_INT(nsent, 1);                                 // and nothing after
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x80, NULL, 0);         // a plain ack gets none
    CHECK_EQ_INT(nsent, 1);
}

static void test_silent_mac_is_probed_then_dropped(void)
{
    int c = we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x60, "?", 1);
    nsent = 0;
    uint64_t t0 = T, at[8] = { 0 }, closed_at = 0;
    int n = 0;
    for (int i = 0; i < 400; i++) {
        T += 100;
        int before = nsent;
        adsp_tick(E, T);
        for (int k = before; k < nsent; k++) if (desc_of(k) == 0xC0 && n < 8) at[n++] = T - t0;
        if (n_closed && !closed_at) closed_at = T - t0;
    }
    CHECK_EQ_INT(n, 3);                                     // a probe each 5 s of silence
    for (int i = 0; i < n; i++) CHECK_EQ_INT(at[i], (uint64_t)(i + 1) * 5000);
    EXPECT(0, &MACD, 0xA003, 0, 2, 0x1000, 0xC0, NULL, 0);
    CHECK_EQ_INT(nsent, 4);                                 // on the fourth, Close Advice
    EXPECT(3, &MACD, 0xA003, 0, 2, 0, 0x85, NULL, 0);
    CHECK_EQ_INT(n_closed, 1);
    CHECK_EQ_INT(last_closed, c);
    CHECK_EQ_INT(last_why, ADSP_END_SILENT);
    CHECK_EQ_INT(closed_at, 20000);
    CHECK(!adsp_is_open(E, c));

    // lpstyl sleeps 15 s without answering, then carries on.
    c = mac_opens(0x0800);
    for (int i = 0; i < 150; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(count_desc(0, 0xC0), 3);
    CHECK(adsp_is_open(E, c));
    uint8_t q[3] = { 0x00, 0x12, 0x00 };
    rx(&MAC, 0x4D01, 0, 0, 0, 0x50, q, 3);                  // an attention counts as heard
    CHECK_EQ_INT(n_attn, 1);
    nsent = 0;
    for (int i = 0; i < 190; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(count_desc(0, 0xC0), 3);                   // the count began again
    CHECK(adsp_is_open(E, c));
    CHECK_EQ_INT(n_closed, 0);
}

static void test_quiet_mac_that_answers_probes_is_kept(void)
{
    int c = we_open(0x0800);
    int probes = 0;
    for (int i = 0; i < 6000; i++) {                        // ten minutes
        T += 100;
        int before = nsent;
        adsp_tick(E, T);
        if (nsent > before) {
            CHECK_EQ_INT(nsent, before + 1);
            CHECK_EQ_INT(desc_of(before), 0xC0);
            probes++;
            rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x80, NULL, 0);
        }
        if (nsent > MAXSENT - 8) nsent = 0;
    }
    CHECK_EQ_INT(probes, 120);
    CHECK(adsp_is_open(E, c));
    CHECK_EQ_INT(n_closed, 0);
}

static void test_forward_reset(void)
{
    we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, "abc", 3);
    nsent = 0;
    // lpstyl's job end: FirstByteSeq is where the stream already is.
    rx(&MACD, 0x4D02, 3, 0, 0x0800, 0x86, NULL, 0);
    CHECK_EQ_INT(nsent, 1);
    EXPECT(0, &MACD, 0xA003, 0, 3, 0x1000, 0x87, NULL, 0);
    rx(&MACD, 0x4D02, 3, 0, 0x0800, 0x60, "\0", 1);         // later data is still accepted
    CHECK_EQ_INT(got_n, 4);
    EXPECT(1, &MACD, 0xA003, 0, 5, 0x1000, 0x80, NULL, 0);

    // Inside the window: the receive sequence moves there.
    rx(&MACD, 0x4D02, 105, 0, 0x0800, 0x86, NULL, 0);
    EXPECT(2, &MACD, 0xA003, 0, 105, 0x1000, 0x87, NULL, 0);
    rx(&MACD, 0x4D02, 5, 0, 0x0800, 0x40, "old", 3);        // what it gave up on
    CHECK_EQ_INT(got_n, 4);
    rx(&MACD, 0x4D02, 105, 0, 0x0800, 0x40, "new", 3);
    CHECK_EQ_INT(got_n, 7);
    EXPECT(4, &MACD, 0xA003, 0, 108, 0x1000, 0x80, NULL, 0);

    // Outside it, or behind: answered, and nothing moves.
    rx(&MACD, 0x4D02, 108 + 0x1001, 0, 0x0800, 0x86, NULL, 0);
    EXPECT(5, &MACD, 0xA003, 0, 108, 0x1000, 0x87, NULL, 0);
    rx(&MACD, 0x4D02, 50, 0, 0x0800, 0x86, NULL, 0);
    EXPECT(6, &MACD, 0xA003, 0, 108, 0x1000, 0x87, NULL, 0);
    rx(&MACD, 0x4D02, 108, 0, 0x0800, 0x40, "!", 1);
    CHECK_EQ_INT(got_n, 8);
    // A Forward Reset Acknowledgement we never asked for: nothing.
    int before = nsent;
    rx(&MACD, 0x4D02, 109, 0, 0x0800, 0x87, NULL, 0);
    CHECK_EQ_INT(nsent, before);
}

static void test_close_advice_from_the_mac(void)
{
    int c = we_open(0x0800);
    adsp_write(E, c, (const uint8_t *)"abc", 3, T);
    nsent = 0;
    rx(&MACD, 0x4D02, 0, 0, 0, 0x85, NULL, 0);
    CHECK_EQ_INT(n_closed, 1);
    CHECK_EQ_INT(last_closed, c);
    CHECK_EQ_INT(last_why, ADSP_END_PEER);
    CHECK_EQ_INT(closed_sock, US);                          // still readable inside closed
    CHECK_EQ_INT(closed_node, 20);
    CHECK(!adsp_is_open(E, c));
    CHECK_EQ_INT(adsp_sock(E, c), 0);
    CHECK_EQ_INT(nsent, 0);                                 // no reply
    // A later packet with that ConnID is ignored, probes included.
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x40, "late", 4);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0xC0, NULL, 0);
    rx(&MACD, 0x4D02, 0, 0, 0, 0x85, NULL, 0);
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    rx(&MACD, 0x4D02, 0, 0, 0, 0x50, q, 3);
    for (int i = 0; i < 300; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(got_n, 0);
    CHECK_EQ_INT(n_attn, 0);
    CHECK_EQ_INT(n_closed, 1);
}

static void test_unknown_conn_id_and_bad_codes_are_ignored(void)
{
    int c = we_open(0x0800);
    adsp_write(E, c, (const uint8_t *)"abc", 3, T);
    nsent = 0;
    uint8_t q[3] = { 0x00, 0x11, 0x00 };
    // Another ConnID, another node, another of the Mac's sockets, another of ours.
    rx(&MACD, 0x4D09, 0, 3, 0x0800, 0x40, "x", 1);
    rx(&MACD, 0x4D09, 0, 0, 0x0800, 0xC0, NULL, 0);
    rx(&MACD, 0x4D09, 0, 0, 0, 0x50, q, 3);
    rx(&MACD, 0x4D09, 0, 0, 0, 0x85, NULL, 0);
    rx(&MAC2, 0x4D02, 0, 3, 0x0800, 0x40, "x", 1);
    rx(&MAC, 0x4D02, 0, 3, 0x0800, 0x40, "x", 1);
    uint8_t p[ADSP_HDR + 1];
    size_t n = pk(p, 0x4D02, 0, 3, 0x0800, 0x40, "x", 1);
    adsp_rx(E, 0x8A, &MACD, p, n, T);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(got_n, 0);
    CHECK_EQ_INT(n_attn, 0);
    CHECK_EQ_INT(n_closed, 0);
    CHECK(!adsp_tx_idle(E, c));
    // Control codes 9 to 15, and packets too short to be one.
    for (uint8_t code = 9; code <= 15; code++) {
        rx(&MACD, 0x4D02, 0, 3, 0x0800, (uint8_t)(0x80 | code), NULL, 0);
        rx(&MACD, 0x4D02, 0, 3, 0x0800, (uint8_t)(0xC0 | code), NULL, 0);
    }
    adsp_rx(E, US, &MACD, p, 12, T);
    adsp_rx(E, US, &MACD, p, 0, T);
    CHECK_EQ_INT(nsent, 0);
    CHECK(!adsp_tx_idle(E, c));
    CHECK(adsp_is_open(E, c));
    // Nothing was hurt.
    rx(&MACD, 0x4D02, 0, 3, 0x0800, 0x40, "y", 1);
    CHECK_EQ_INT(got_n, 1);
    CHECK(adsp_tx_idle(E, c));
}

static void test_our_close_and_reset(void)
{
    int c = we_open(0x0800);
    rx(&MACD, 0x4D02, 0, 0, 0x0800, 0x00, "ab", 2);
    adsp_write(E, c, (const uint8_t *)"xyz", 3, T);
    nsent = 0;
    adsp_close(E, c, T);
    CHECK_EQ_INT(nsent, 1);                                 // Close Advice once
    EXPECT(0, &MACD, 0xA003, 3, 2, 0, 0x85, NULL, 0);
    CHECK_EQ_INT(n_closed, 0);                              // and closed is not called
    CHECK(!adsp_is_open(E, c));
    adsp_close(E, c, T);
    for (int i = 0; i < 300; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(n_closed, 0);

    // Closing an open still on its way stops the requests.
    fresh(0xA002);
    c = adsp_open(E, US, &MACD, T);
    adsp_close(E, c, T);
    CHECK_EQ_INT(nsent, 2);
    CHECK_EQ_INT(desc_of(1), 0x85);
    for (int i = 0; i < 100; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(nsent, 2);
    CHECK_EQ_INT(n_closed, 0);

    // adsp_reset forgets every connection and sends nothing.
    fresh(0xA000);
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    rx_open(&MAC2, 0x4D01, 0x0800, 0x81, 0);
    c = adsp_open(E, US, &MACD, T);
    adsp_write(E, 0, (const uint8_t *)"abc", 3, T);
    nsent = 0;
    adsp_reset(E);
    for (int i = 0; i < ADSP_CONNS; i++) CHECK(!adsp_is_open(E, i) && adsp_sock(E, i) == 0);
    for (int i = 0; i < 300; i++) { T += 100; adsp_tick(E, T); }
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(n_closed, 0);
    rx_open(&MAC, 0x4D05, 0x0800, 0x81, 0);                 // and it still works after
    CHECK_EQ_INT(nsent, 1);
    CHECK_EQ_INT(n_opened, 3);
}

static void test_open_from_inside_closed(void)
{
    // The adapter calls the Mac back when its control connection closes, with
    // the table full: the place just closed is the one to use.
    fresh(0xA000);
    for (int i = 0; i < ADSP_CONNS - 1; i++) {
        lt_route_t m = { { 0, (uint8_t)(30 + i), 0xE0 }, (uint8_t)(30 + i), 0 };
        rx_open(&m, 0x4D01, 0x0800, 0x81, 0);
    }
    rx_open(&MAC, 0x4D01, 0x0800, 0x81, 0);
    int c = last_opened;
    CHECK_EQ_INT(n_opened, ADSP_CONNS);
    nsent = 0;
    reopen_on_close = 1;
    rx(&MAC, 0x4D01, 0, 0, 0, 0x85, NULL, 0);
    CHECK_EQ_INT(n_closed, 1);
    CHECK_EQ_INT(reopened, c);
    CHECK_EQ_INT(nsent, 1);
    EXPECT_OPEN(0, &MACD, 0xA005, 0x1000, 0x81, 0);
    CHECK_EQ_INT(adsp_sock(E, c), US);                      // it is the new open's now
    CHECK(!adsp_is_open(E, c));
    reopen_on_close = 0;
    rx_open(&MACD, 0x4D02, 0x0800, 0x83, 0xA005);
    CHECK(adsp_is_open(E, c));
    CHECK(adsp_peer(E, c) && adsp_peer(E, c)->addr.sock == 0xF1);
    // The old connection's packets are nobody's.
    nsent = 0;
    rx(&MAC, 0x4D01, 0, 0, 0x0800, 0x40, "x", 1);
    CHECK_EQ_INT(nsent, 0);
    CHECK_EQ_INT(got_n, 0);
}

int main(void)
{
    test_codec_vectors();
    test_mac_opens_to_us();
    test_repeated_open_request_gets_the_same_answer();
    test_open_works_without_the_third_leg();
    test_two_macs_with_one_conn_id_are_two_connections();
    test_open_request_nobody_wants_gets_silence();
    test_we_open_to_the_mac();
    test_open_to_a_silent_mac_gives_up();
    test_open_denied_gives_up_at_once();
    test_second_request_ack_from_a_new_conn_id_is_adopted();
    test_repeated_request_ack_gets_the_ack_again();
    test_conn_ids();
    test_attention();
    test_repeated_attention_is_acked_not_delivered();
    test_attention_without_ack_request_is_acked();
    test_attention_out_of_sequence();
    test_attention_leaves_the_stream_alone();
    test_data_both_ways();
    test_message_across_two_packets();
    test_bare_eom();
    test_data_without_ack_request_is_not_acked();
    test_duplicate_ahead_and_overlap();
    test_bytes_in_order_across_packets_and_messages();
    test_write_with_no_connection_sends_nothing();
    test_bytes_beyond_room_are_not_taken();
    test_window_shut_then_opened_is_announced();
    test_window_filled_without_an_ack_is_still_announced();
    test_far_window_zero_holds_our_bytes();
    test_far_window_of_three();
    test_lost_data_comes_again();
    test_retransmit_advice();
    test_probe_gets_one_ack();
    test_silent_mac_is_probed_then_dropped();
    test_quiet_mac_that_answers_probes_is_kept();
    test_forward_reset();
    test_close_advice_from_the_mac();
    test_unknown_conn_id_and_bad_codes_are_ignored();
    test_our_close_and_reset();
    test_open_from_inside_closed();
    free(E);
    free(sent);
    TEST_END;
}
