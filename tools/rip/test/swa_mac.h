// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Mac's StyleWriter driver for the tests that run the adapter under a node
// (W-1193): test_localtalk.c on a cable node, test_ethertalk.c through an
// et_link. The adapter rides the node (lt_node_attach_swa) with a fake
// printer behind its job calls; the Mac here speaks ADSP in datagrams that
// the including test wraps in its own link's frames, and reads what the node
// sent back. test_sw_adapter.c holds the adapter itself to the byte; this
// holds that a whole print gets through the node both ways.
//
// The including test defines, after this header:
//   swm_in(mac_sock, our_sock, p, n)  one ADSP packet from the Mac's socket to
//                                     ours, as a frame into the node, at its clock
//   swm_step(ms)                      time passes: the node is ticked
// and calls swm_out() from its wire for every DDP type 7 datagram the node sends.
#pragma once
#include "test_util.h"
#include "lt_proto.h"
#include "lt_node.h"
#include "adsp.h"
#include "sw_adapter.h"

static void swm_in(uint8_t mac_sock, uint8_t our_sock, const uint8_t *p, size_t n);
static void swm_step(unsigned ms);

// ------------------------------------------------------------- what the node sent

#define SWM_MAXOUT 128
typedef struct {
    uint8_t to_sock, from_sock;
    size_t  n;
    uint8_t p[ADSP_HDR + ADSP_MAX_DATA];
} swm_out_t;
static swm_out_t swm_sent[SWM_MAXOUT];
static int       swm_nsent;

static void swm_out(const ddp_t *d)
{
    CHECK(d->type == DDP_ADSP && swm_nsent < SWM_MAXOUT && d->len <= sizeof swm_sent[0].p);
    if (swm_nsent >= SWM_MAXOUT || d->len > sizeof swm_sent[0].p) return;
    swm_out_t *o = &swm_sent[swm_nsent++];
    o->to_sock = d->dst.sock;
    o->from_sock = d->src.sock;
    o->n = d->len;
    memcpy(o->p, d->data, d->len);
}

// ------------------------------------------------------------- the board behind the adapter

static sw_adapter_t swm_adapter;
static lt_node_t   *swm_node;
static int     swm_begins, swm_begin_id, swm_begin_ret, swm_ups, swm_ends, swm_end_clean, swm_drained;
static uint8_t swm_printer[256];
static size_t  swm_printer_n;
static uint8_t swm_back[64];
static size_t  swm_back_n, swm_back_at;

static void swm_send(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n)
{
    (void)ctx;
    lt_node_send_ddp(swm_node, to, src_sock, DDP_ADSP, p, n);
}
static void swm_log(void *ctx, const char *line)
{
    (void)ctx;
    CHECK(!strstr(line, "Bob") && !strstr(line, "Inkwell"));    // no person's or printer's name
}
static int swm_busy(void *ctx) { (void)ctx; return 0; }
static int swm_begin(void *ctx, uint8_t id) { (void)ctx; swm_begins++; swm_begin_id = id; return swm_begin_ret; }
static void swm_up(void *ctx) { (void)ctx; swm_ups++; }
static size_t swm_room(void *ctx) { (void)ctx; return 0x1000; }
static void swm_data(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (swm_printer_n + n <= sizeof swm_printer) memcpy(swm_printer + swm_printer_n, d, n);
    swm_printer_n += n;
}
static size_t swm_output(void *ctx, uint8_t *buf, size_t max)
{
    (void)ctx;
    size_t n = swm_back_n - swm_back_at;
    if (n > max) n = max;
    memcpy(buf, swm_back + swm_back_at, n);
    swm_back_at += n;
    return n;
}
static int swm_is_drained(void *ctx) { (void)ctx; return swm_drained; }
static void swm_end(void *ctx, int clean) { (void)ctx; swm_ends++; swm_end_clean = clean; }
static void swm_rename(void *ctx, uint8_t id, const uint8_t *pname) { (void)ctx; (void)id; (void)pname; }

static void swm_printer_says(const void *d, size_t n)
{
    memcpy(swm_back + swm_back_n, d, n);
    swm_back_n += n;
}

// A fresh adapter on node n, offering "Inkwell" as a colour StyleWriter (record 3) and
// "Quill" as a black one (record 5).
static void swm_attach(lt_node_t *n, uint64_t now)
{
    static const swa_ops_t ops = { NULL, swm_send, swm_log, swm_busy, swm_begin, swm_up, swm_room,
                                   swm_data, swm_output, swm_is_drained, swm_end, swm_rename };
    swa_name_t names[SWA_NAMES];
    memset(names, 0, sizeof names);
    nbp_name(names[0].obj, "Inkwell");
    nbp_name(names[0].type, SWA_TYPE_COLOR);
    names[0].id = 3;
    nbp_name(names[1].obj, "Quill");
    nbp_name(names[1].type, SWA_TYPE_MONO);
    names[1].id = 5;
    swm_node = n;
    swm_nsent = 0;
    swm_begins = swm_begin_id = swm_begin_ret = swm_ups = swm_ends = 0;
    swm_end_clean = -1;
    swm_drained = 1;
    swm_printer_n = swm_back_n = swm_back_at = 0;
    swa_init(&swm_adapter, &ops, 0xA000);
    swa_offer(&swm_adapter, names, SWA_NAMES, now);
    lt_node_attach_swa(n, &swm_adapter);
}

// ------------------------------------------------------------- one of the Mac's connections

typedef struct {
    uint8_t  sock, ours;              // its socket, and ours
    uint16_t id, far, win;
    uint32_t tx, rx, attn;
    int      mark, closed;
    uint8_t  in[512];                 // stream bytes read since the last question
    size_t   in_n;
} swm_conn_t;

static void swm_pkt(swm_conn_t *m, uint32_t first, uint32_t next, uint16_t win, uint8_t desc,
                    const void *d, size_t n)
{
    uint8_t p[ADSP_HDR + ADSP_MAX_DATA];
    adsp_hdr_t h = { m->id, first, next, win, desc };
    adsp_hdr_build(p, &h);
    if (n) memcpy(p + ADSP_HDR, d, n);
    swm_in(m->sock, m->ours, p, ADSP_HDR + n);
}

static void swm_open_pkt(swm_conn_t *m, uint8_t desc)
{
    uint8_t par[8] = { 0x01, 0x00, (uint8_t)(m->far >> 8), (uint8_t)m->far, 0, 0, 0, 0 };
    swm_pkt(m, 0, 0, m->win, desc, par, sizeof par);
}

// Read what the adapter has sent this connection since the last look.
static void swm_poll(swm_conn_t *m)
{
    for (; m->mark < swm_nsent; m->mark++) {
        const swm_out_t *s = &swm_sent[m->mark];
        adsp_hdr_t h;
        if (s->to_sock != m->sock || s->from_sock != m->ours) continue;
        if (adsp_hdr_parse(s->p, s->n, &h) || h.id != m->far) continue;
        if (h.desc == 0x85) m->closed = 1;
        if (h.desc & 0x90) continue;                // control and attention acks: no stream bytes
        size_t n = s->n - ADSP_HDR;
        if (h.first != m->rx || m->in_n + n > sizeof m->in) continue;
        memcpy(m->in + m->in_n, s->p + ADSP_HDR, n);
        m->in_n += n;
        m->rx += (uint32_t)n;
    }
}

static void swm_ack(swm_conn_t *m) { swm_pkt(m, m->tx, m->rx, m->win, 0x80, NULL, 0); }

// The Mac opens a connection from its socket to ours: 1 when we answered Request+Ack.
static int swm_open(swm_conn_t *m, uint8_t sock, uint8_t ours, uint16_t id)
{
    memset(m, 0, sizeof *m);
    m->sock = sock; m->ours = ours; m->id = id; m->win = 0x0800;
    int before = swm_nsent;
    swm_open_pkt(m, 0x81);
    m->mark = swm_nsent;
    if (swm_nsent != before + 1 || swm_sent[before].p[12] != 0x83 ||
        swm_sent[before].to_sock != sock || swm_sent[before].from_sock != ours)
        return 0;
    m->far = (uint16_t)(swm_sent[before].p[0] << 8 | swm_sent[before].p[1]);
    swm_open_pkt(m, 0x82);
    return 1;
}

// The Mac takes our call to its socket: the first Open Request there from index `from` on
// is answered. 1 when our third leg came back.
static int swm_called(swm_conn_t *m, uint8_t sock, uint8_t ours, uint16_t id, int from)
{
    memset(m, 0, sizeof *m);
    m->sock = sock; m->ours = ours; m->id = id; m->win = 0x0800;
    for (int i = from; i < swm_nsent; i++) {
        if (swm_sent[i].to_sock != sock || swm_sent[i].from_sock != ours || swm_sent[i].p[12] != 0x81)
            continue;
        m->far = (uint16_t)(swm_sent[i].p[0] << 8 | swm_sent[i].p[1]);
        int before = swm_nsent;
        swm_open_pkt(m, 0x83);
        m->mark = swm_nsent;
        return swm_nsent == before + 1 && swm_sent[before].p[12] == 0x82;
    }
    return 0;
}

// An attention message, and the 2 bytes that answer it on the stream.
static void swm_ask(swm_conn_t *m, uint16_t code, const void *body, size_t n, uint8_t b0, uint8_t b1)
{
    uint8_t d[2 + 300] = { (uint8_t)(code >> 8), (uint8_t)code };
    swm_poll(m);
    m->in_n = 0;
    if (n) memcpy(d + 2, body, n);
    swm_pkt(m, m->attn++, 0, 0, 0x50, d, n + 2);
    swm_poll(m);
    swm_ack(m);
    CHECK_EQ_INT(m->in_n, 2);
    CHECK_EQ_INT(m->in[0], b0);
    CHECK_EQ_INT(m->in[1], b1);
}

// A message of the Mac's stream (end of message set, as a driver's write is).
static void swm_write(swm_conn_t *m, const void *d, size_t n)
{
    swm_pkt(m, m->tx, m->rx, m->win, 0x60, d, n);
    m->tx += (uint32_t)n + 1;
}

// The printer's bytes as they reach the Mac: n of them, equal to want.
static void swm_reads(swm_conn_t *m, const void *want, size_t n)
{
    m->in_n = 0;
    swm_step(20);
    swm_poll(m);
    swm_ack(m);
    CHECK_EQ_INT(m->in_n, n);
    if (m->in_n == n) CHECK_EQ_MEM(m->in, want, n);
}

// ------------------------------------------------------------- the whole print

// Apple's order (the protocol brief's D.8), from the Mac's sockets E0 (control) and F1
// (data) to the name on our socket `ours`, whose record is `id`: the request, the session
// end and close, our call back, a status text, the reset and two questions with the
// printer's answers, the drained question, the end. Nothing is left open or held.
static void swm_whole_print(uint8_t ours, int id)
{
    static uint8_t req[SWA_REQ_LEN], status[SWA_STATUS_LEN];
    memset(req, 0, sizeof req);
    memcpy(req, "\x00\x00\x00\xF1\x03" "Bob", 8);
    memset(status, 0, sizeof status);
    memcpy(status, "\x06\x47\x08" "Printing", 11);
    int begins = swm_begins, ups = swm_ups, ends = swm_ends;
    swm_printer_n = 0;

    swm_conn_t ctl, pipe;
    CHECK(swm_open(&ctl, 0xE0, ours, 0x4D01));
    swm_ask(&ctl, SWA_PRINT, req, sizeof req, 0x00, 0x00);
    CHECK_EQ_INT(swm_begins, begins + 1);
    CHECK_EQ_INT(swm_begin_id, id);
    CHECK_EQ_INT(swa_job(&swm_adapter), 1);
    int from = swm_nsent;
    swm_ask(&ctl, SWA_END, "\0", 1, 0x00, 0x00);
    swm_pkt(&ctl, ctl.tx, ctl.rx, 0, 0x85, NULL, 0);            // the Mac closes
    CHECK(swm_called(&pipe, 0xF1, ours, 0x4D02, from));         // our call, from the same socket
    CHECK_EQ_INT(swm_ups, ups + 1);
    CHECK_EQ_INT(swa_job(&swm_adapter), 2);

    swm_ask(&pipe, SWA_SET_STATUS, status, sizeof status, 0x00, 0x00);
    swm_write(&pipe, "\x00", 1);
    swm_write(&pipe, "\xFF\xFF\xFF\x49", 4);
    swm_step(2000);
    swm_write(&pipe, "\xFF\xFF\xFF\x31", 4);
    swm_printer_says("\x08", 1);
    swm_reads(&pipe, "\x08", 1);
    swm_write(&pipe, "\x3F", 1);
    swm_printer_says("CS\r", 3);
    swm_reads(&pipe, "CS\r", 3);

    swm_drained = 0;
    swm_ask(&pipe, SWA_DRAINED, "\0", 1, 0x00, 0x00);
    swm_drained = 1;
    swm_ask(&pipe, SWA_DRAINED, "\0", 1, 0xFF, 0xFF);
    swm_ask(&pipe, SWA_END, "\0", 1, 0xFF, 0xFF);
    CHECK_EQ_INT(swm_ends, ends);
    swm_pkt(&pipe, pipe.tx, pipe.rx, 0, 0x85, NULL, 0);
    CHECK_EQ_INT(swm_ends, ends + 1);
    CHECK_EQ_INT(swm_end_clean, 1);
    CHECK_EQ_INT(swa_job(&swm_adapter), 0);

    // The printer got what the Mac sent, in order.
    CHECK_EQ_INT(swm_printer_n, 10);
    CHECK_EQ_MEM(swm_printer, "\x00\xFF\xFF\xFF\x49\xFF\xFF\xFF\x31\x3F", 10);
    // And nothing more comes of it.
    int quiet = swm_nsent;
    swm_step(30000);
    CHECK_EQ_INT(swm_nsent, quiet);
    CHECK_EQ_INT(swm_ends, ends + 1);
}
