// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADSP (W-1193): see adsp.h. Sequence numbers are 32 bits and wrap, so every
// comparison is a difference.
#include "adsp.h"
#include <string.h>

enum { ST_FREE = 0, ST_DYING, ST_OPENING, ST_OPEN };

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

int adsp_hdr_parse(const uint8_t *p, size_t n, adsp_hdr_t *h)
{
    if (n < ADSP_HDR) return -1;
    h->id    = get16(p);
    h->first = get32(p + 2);
    h->next  = get32(p + 6);
    h->win   = get16(p + 10);
    h->desc  = p[12];
    if ((h->desc & ADSP_CONTROL) && (h->desc & ADSP_CODE_MASK) > ADSP_RETRANSMIT) return -1;
    return 0;
}

void adsp_hdr_build(uint8_t out[ADSP_HDR], const adsp_hdr_t *h)
{
    put16(out, h->id);
    put32(out + 2, h->first);
    put32(out + 6, h->next);
    put16(out + 10, h->win);
    out[12] = h->desc;
}

// ------------------------------------------------------------- connections

static adsp_conn_t *conn(const adsp_t *a, int i)
{
    if (i < 0 || i >= ADSP_CONNS) return NULL;
    return (adsp_conn_t *)&a->c[i];
}

static int same_addr(const lt_addr_t *x, const lt_addr_t *y)
{
    return x->net == y->net && x->node == y->node && x->sock == y->sock;
}

// The open connection these packets belong to: our socket, the far address
// and the far connection ID together.
static int find(const adsp_t *a, uint8_t local_sock, const lt_route_t *from, uint16_t far_id)
{
    for (int i = 0; i < ADSP_CONNS; i++) {
        const adsp_conn_t *c = &a->c[i];
        if (c->state == ST_OPEN && c->local_sock == local_sock && c->far_id == far_id &&
            same_addr(&c->peer.addr, &from->addr))
            return i;
    }
    return -1;
}

// The connection we opened, by our own ID (the far end names it in its answer).
static int find_ours(const adsp_t *a, uint8_t local_sock, uint16_t id)
{
    for (int i = 0; i < ADSP_CONNS; i++) {
        const adsp_conn_t *c = &a->c[i];
        if (c->state >= ST_OPENING && c->opener && c->local_sock == local_sock && c->id == id)
            return i;
    }
    return -1;
}

// A place for a new connection: a free one, or the one being closed around us.
static int place(const adsp_t *a)
{
    for (int i = 0; i < ADSP_CONNS; i++) if (a->c[i].state == ST_FREE) return i;
    for (int i = 0; i < ADSP_CONNS; i++) if (a->c[i].state == ST_DYING) return i;
    return -1;
}

static uint16_t new_id(adsp_t *a)
{
    for (;;) {
        uint16_t id = ++a->next_id;
        int used = id == 0;
        for (int i = 0; i < ADSP_CONNS && !used; i++)
            used = a->c[i].state != ST_FREE && a->c[i].id == id;
        if (!used) return id;
    }
}

static adsp_conn_t *begin(adsp_t *a, int i, uint8_t local_sock, const lt_route_t *peer, uint64_t now)
{
    adsp_conn_t *c = &a->c[i];
    uint16_t id = new_id(a);
    memset(c, 0, sizeof *c);
    c->id = id;
    c->local_sock = local_sock;
    c->peer = *peer;
    c->heard = c->timer = c->tx_at = now;
    return c;
}

// Still the open connection it was before a call out to the owner.
static int alive(const adsp_conn_t *c, uint16_t id) { return c->state == ST_OPEN && c->id == id; }

// The connection is over, not by adsp_close: tell the owner. Its place can be
// taken by an adsp_open made in the call.
static void end(adsp_t *a, int i, adsp_end_t why)
{
    adsp_conn_t *c = &a->c[i];
    uint16_t id = c->id;
    c->state = ST_DYING;
    if (a->ops.closed) a->ops.closed(a->ops.ctx, i, why);
    if (c->state == ST_DYING && c->id == id) c->state = ST_FREE;
}

// ------------------------------------------------------------- packets out

static uint16_t window(adsp_t *a, int i)
{
    size_t r = a->ops.room ? a->ops.room(a->ops.ctx, i) : 0;
    return (uint16_t)(r > 0xFFFF ? 0xFFFF : r);
}

static void out(adsp_t *a, const adsp_conn_t *c, const uint8_t *p, size_t n)
{
    if (a->ops.send) a->ops.send(a->ops.ctx, &c->peer, c->local_sock, p, n);
}

// A stream packet: the sequence of its first byte, our ack, our window.
static void send_stream(adsp_t *a, int i, uint8_t desc, uint32_t first, const uint8_t *d, size_t n)
{
    adsp_conn_t *c = &a->c[i];
    uint8_t p[ADSP_HDR + ADSP_MAX_DATA];
    adsp_hdr_t h = { c->id, first, c->rx_seq, window(a, i), desc };
    if (n > ADSP_MAX_DATA) return;
    c->rx_limit = c->rx_seq + h.win;
    adsp_hdr_build(p, &h);
    if (n) memcpy(p + ADSP_HDR, d, n);
    out(a, c, p, ADSP_HDR + n);
}

static void send_control(adsp_t *a, int i, uint8_t desc)
{
    const adsp_conn_t *c = &a->c[i];
    send_stream(a, i, desc, c->tx_una + c->tx_sent, NULL, 0);
}

// Close Advice carries no window: nothing more is taken.
static void send_close(adsp_t *a, int i)
{
    const adsp_conn_t *c = &a->c[i];
    uint8_t p[ADSP_HDR];
    adsp_hdr_t h = { c->id, c->tx_una + c->tx_sent, c->rx_seq, 0, ADSP_CONTROL | ADSP_CLOSE };
    adsp_hdr_build(p, &h);
    out(a, c, p, sizeof p);
}

static void send_open(adsp_t *a, int i, uint8_t code)
{
    adsp_conn_t *c = &a->c[i];
    uint8_t p[ADSP_HDR + ADSP_OPEN_PARAMS];
    adsp_hdr_t h = { c->id, c->tx_una, c->rx_seq, window(a, i), (uint8_t)(ADSP_CONTROL | code) };
    c->rx_limit = c->rx_seq + h.win;
    adsp_hdr_build(p, &h);
    put16(p + ADSP_HDR, ADSP_VERSION);
    put16(p + ADSP_HDR + 2, code == ADSP_OPEN_REQ ? 0 : c->far_id);
    put32(p + ADSP_HDR + 4, c->attn_rx);
    out(a, c, p, sizeof p);
}

// An attention's ack: 13 bytes, the attention sequences, no window. We send
// no attentions, so our own attention sequence stays 0.
static void send_attn_ack(adsp_t *a, int i)
{
    const adsp_conn_t *c = &a->c[i];
    uint8_t p[ADSP_HDR];
    adsp_hdr_t h = { c->id, 0, c->attn_rx, 0, ADSP_CONTROL | ADSP_ATTENTION };
    adsp_hdr_build(p, &h);
    out(a, c, p, sizeof p);
}

// ------------------------------------------------------------- our bytes

// Send what is kept and not yet sent, as far as the far window allows.
static void pump(adsp_t *a, int i, uint64_t now)
{
    adsp_conn_t *c = &a->c[i];
    while (c->state == ST_OPEN && c->tx_sent < c->tx_len) {
        uint32_t at = c->tx_una + c->tx_sent;
        int32_t  allowed = (int32_t)(c->tx_limit - at);
        size_t   n = (size_t)(c->tx_len - c->tx_sent);
        if (allowed <= 0) break;
        if (n > ADSP_MAX_DATA) n = ADSP_MAX_DATA;
        if (n > (size_t)allowed) n = (size_t)allowed;
        send_stream(a, i, ADSP_ACK_REQ, at, c->tx + c->tx_sent, n);
        c->tx_sent = (uint16_t)(c->tx_sent + n);
        c->tx_at = now;
    }
}

// Everything sent and unacknowledged, once more.
static void resend(adsp_t *a, int i, uint64_t now)
{
    adsp_conn_t *c = &a->c[i];
    for (size_t at = 0; at < c->tx_sent; ) {
        size_t n = c->tx_sent - at;
        if (n > ADSP_MAX_DATA) n = ADSP_MAX_DATA;
        send_stream(a, i, ADSP_ACK_REQ, c->tx_una + (uint32_t)at, c->tx + at, n);
        at += n;
    }
    c->tx_at = now;
}

// A stream packet's ack and window. An ack that is behind what is already
// acknowledged, or beyond what was sent, is stale or wrong: it and its
// window are left alone.
static void apply_ack(adsp_conn_t *c, const adsp_hdr_t *h)
{
    uint32_t k = h->next - c->tx_una;
    if (k > c->tx_sent) return;
    if (k) {
        memmove(c->tx, c->tx + k, c->tx_len - k);
        c->tx_len = (uint16_t)(c->tx_len - k);
        c->tx_sent = (uint16_t)(c->tx_sent - k);
        c->tx_una += k;
        c->resends = 0;
        c->fresh = 0;
    }
    c->tx_limit = h->next + h->win;
}

// ------------------------------------------------------------- packets in

static void rx_open(adsp_t *a, uint8_t local_sock, const lt_route_t *from, const adsp_hdr_t *h,
                    const uint8_t *par, size_t n, uint64_t now)
{
    if (n < ADSP_OPEN_PARAMS) return;
    uint16_t dest = get16(par + 2);
    int i;
    adsp_conn_t *c;

    switch (h->desc & ADSP_CODE_MASK) {
    case ADSP_OPEN_REQ:
        i = find(a, local_sock, from, h->id);
        if (i >= 0) {                                   // our answer was lost: the same again
            c = &a->c[i];
            if (c->opener) return;
            c->heard = now; c->probes = 0;
            send_open(a, i, ADSP_OPEN_REQ_ACK);
            return;
        }
        i = place(a);
        if (i < 0) return;
        if (!a->ops.accept || !a->ops.accept(a->ops.ctx, local_sock, from)) return;
        c = begin(a, i, local_sock, from, now);
        c->state = ST_OPEN;
        c->fresh = 1;
        c->far_id = h->id;
        c->rx_seq = h->first;
        c->tx_limit = c->tx_una + h->win;
        send_open(a, i, ADSP_OPEN_REQ_ACK);
        if (a->ops.opened) a->ops.opened(a->ops.ctx, i);
        return;

    case ADSP_OPEN_REQ_ACK:
        i = find_ours(a, local_sock, dest);
        if (i < 0) return;
        c = &a->c[i];
        if (c->state == ST_OPENING) {
            c->state = ST_OPEN;
            c->fresh = 1;
            c->far_id = h->id;
            c->peer = *from;
            c->rx_seq = h->first;
            c->tx_limit = c->tx_una + h->win;
            c->heard = c->timer = now;
            c->probes = 0;
            send_open(a, i, ADSP_OPEN_ACK);
            if (a->ops.opened) a->ops.opened(a->ops.ctx, i);
            return;
        }
        if (c->far_id == h->id && same_addr(&c->peer.addr, &from->addr)) {
            c->heard = now; c->probes = 0;              // our ack was lost
            send_open(a, i, ADSP_OPEN_ACK);
            return;
        }
        // A far end that answered our repeated request with a second
        // connection uses only the newer one: so do we, until bytes have come.
        if (!c->fresh) return;
        c->far_id = h->id;
        c->peer = *from;
        c->rx_seq = h->first;
        c->tx_limit = c->tx_una + h->win;
        c->tx_sent = 0;
        c->heard = now; c->probes = 0;
        send_open(a, i, ADSP_OPEN_ACK);
        pump(a, i, now);
        return;

    case ADSP_OPEN_ACK:                                 // the third leg: nothing to answer
        i = find(a, local_sock, from, h->id);
        if (i < 0) return;
        c = &a->c[i];
        c->heard = now; c->probes = 0;
        apply_ack(c, h);
        pump(a, i, now);
        return;

    case ADSP_OPEN_DENY:
        i = find_ours(a, local_sock, dest);
        if (i >= 0 && a->c[i].state == ST_OPENING) end(a, i, ADSP_END_OPEN);
        return;
    }
}

static void rx_attention(adsp_t *a, int i, const adsp_hdr_t *h, const uint8_t *d, size_t n)
{
    adsp_conn_t *c = &a->c[i];
    if (n < 2 || n - 2 > ADSP_ATTN_MAX) return;
    if (h->first != c->attn_rx) {                       // a repeat, its ack lost
        send_attn_ack(a, i);
        return;
    }
    c->attn_rx++;
    c->fresh = 0;
    send_attn_ack(a, i);                                // before the owner's answer
    if (a->ops.attention) a->ops.attention(a->ops.ctx, i, get16(d), d + 2, n - 2);
}

// Stream bytes: in order only. 0 when the owner ended the connection in its call.
static int rx_data(adsp_t *a, int i, const adsp_hdr_t *h, const uint8_t *d, size_t n)
{
    adsp_conn_t *c = &a->c[i];
    uint16_t id = c->id;
    uint32_t had = c->rx_seq - h->first;                // bytes of this packet we already have
    if ((int32_t)had < 0 || had > n) return 1;          // ahead, or all of it old
    size_t more = n - had, take = more;
    size_t room = a->ops.room ? a->ops.room(a->ops.ctx, i) : 0;
    if (take > room) take = room;
    int eom = (h->desc & ADSP_EOM) && take == more;     // the end counts once every byte is in
    if (!take && !eom) return 1;
    c->rx_seq += (uint32_t)take + (eom ? 1 : 0);
    c->fresh = 0;
    if (a->ops.data) a->ops.data(a->ops.ctx, i, d + had, take, eom);
    return alive(c, id);
}

void adsp_rx(adsp_t *a, uint8_t local_sock, const lt_route_t *from,
             const uint8_t *p, size_t n, uint64_t now)
{
    adsp_hdr_t h;
    if (adsp_hdr_parse(p, n, &h)) return;
    const uint8_t *d = p + ADSP_HDR;
    size_t dn = n - ADSP_HDR;
    uint8_t code = h.desc & ADSP_CODE_MASK;
    int control = (h.desc & ADSP_CONTROL) != 0;

    if (h.desc & ADSP_ATTENTION) {
        // An attention message, or the ack of one. Neither reaches the
        // stream's ack or window code: their sequence fields are the
        // attention's and their window is 0.
        int i = find(a, local_sock, from, h.id);
        if (i < 0) return;
        a->c[i].heard = now; a->c[i].probes = 0;
        if (!control) rx_attention(a, i, &h, d, dn);
        return;
    }
    if (control && code >= ADSP_OPEN_REQ && code <= ADSP_OPEN_DENY) {
        rx_open(a, local_sock, from, &h, d, dn, now);
        return;
    }

    int i = find(a, local_sock, from, h.id);
    if (i < 0) return;
    adsp_conn_t *c = &a->c[i];
    c->heard = now; c->probes = 0;

    if (control) {
        switch (code) {
        case ADSP_CLOSE:
            end(a, i, ADSP_END_PEER);
            return;
        case ADSP_FWD_RESET:
            apply_ack(c, &h);
            if (h.first - c->rx_seq <= c->rx_limit - c->rx_seq) c->rx_seq = h.first;
            send_control(a, i, ADSP_CONTROL | ADSP_FWD_RESET_ACK);
            pump(a, i, now);
            return;
        case ADSP_RETRANSMIT:
            apply_ack(c, &h);
            c->tx_sent = 0;                             // from there, at once
            pump(a, i, now);
            return;
        default:                                        // an ack or a probe, a reset's ack
            break;
        }
    } else if (!rx_data(a, i, &h, d, dn)) {
        return;
    }
    apply_ack(c, &h);
    if (h.desc & ADSP_ACK_REQ) send_control(a, i, ADSP_CONTROL | ADSP_PROBE_OR_ACK);
    pump(a, i, now);
}

// ------------------------------------------------------------- the owner's calls

void adsp_init(adsp_t *a, const adsp_ops_t *ops, uint32_t seed)
{
    memset(a, 0, sizeof *a);
    a->ops = *ops;
    a->next_id = (uint16_t)seed;
}

void adsp_reset(adsp_t *a)
{
    memset(a->c, 0, sizeof a->c);
}

int adsp_open(adsp_t *a, uint8_t local_sock, const lt_route_t *to, uint64_t now)
{
    int i = place(a);
    if (i < 0) return -1;
    adsp_conn_t *c = begin(a, i, local_sock, to, now);
    c->state = ST_OPENING;
    c->opener = 1;
    send_open(a, i, ADSP_OPEN_REQ);
    return i;
}

size_t adsp_write(adsp_t *a, int i, const uint8_t *d, size_t n, uint64_t now)
{
    adsp_conn_t *c = conn(a, i);
    if (!c || c->state != ST_OPEN) return 0;
    size_t space = (size_t)(ADSP_TX_CAP - c->tx_len);
    if (n > space) n = space;
    if (!n) return 0;
    memcpy(c->tx + c->tx_len, d, n);
    c->tx_len = (uint16_t)(c->tx_len + n);
    pump(a, i, now);
    return n;
}

size_t adsp_tx_free(const adsp_t *a, int i)
{
    const adsp_conn_t *c = conn(a, i);
    return c && c->state == ST_OPEN ? (size_t)(ADSP_TX_CAP - c->tx_len) : 0;
}

int adsp_tx_idle(const adsp_t *a, int i)
{
    const adsp_conn_t *c = conn(a, i);
    return !c || c->state != ST_OPEN || c->tx_len == 0;
}

// The far end may send up to rx_limit. Once it has, only we can tell it
// there is room again: it has nothing left to send that would draw the news.
void adsp_window(adsp_t *a, int i, uint64_t now)
{
    adsp_conn_t *c = conn(a, i);
    (void)now;
    if (!c || c->state != ST_OPEN || c->rx_limit != c->rx_seq) return;
    if (window(a, i)) send_control(a, i, ADSP_CONTROL | ADSP_PROBE_OR_ACK);
}

void adsp_close(adsp_t *a, int i, uint64_t now)
{
    adsp_conn_t *c = conn(a, i);
    (void)now;
    if (!c || c->state < ST_OPENING) return;
    send_close(a, i);
    c->state = ST_FREE;
}

void adsp_tick(adsp_t *a, uint64_t now)
{
    for (int i = 0; i < ADSP_CONNS; i++) {
        adsp_conn_t *c = &a->c[i];
        if (c->state == ST_OPENING) {
            if (now - c->timer < ADSP_OPEN_RETRY_MS) continue;
            if (c->tries >= ADSP_OPEN_RETRIES) { end(a, i, ADSP_END_OPEN); continue; }
            c->tries++;
            c->timer = now;
            send_open(a, i, ADSP_OPEN_REQ);
            continue;
        }
        if (c->state != ST_OPEN) continue;
        if (c->tx_sent && now - c->tx_at >= ADSP_RESEND_MS) {
            if (c->resends >= ADSP_RESENDS) {
                send_close(a, i);
                end(a, i, ADSP_END_RESEND);
                continue;
            }
            c->resends++;
            resend(a, i, now);
        }
        uint64_t quiet = c->heard > c->timer ? c->heard : c->timer;
        if (now - quiet >= ADSP_PROBE_MS) {
            if (c->probes >= ADSP_PROBE_EXPIRIES - 1) {
                send_close(a, i);
                end(a, i, ADSP_END_SILENT);
                continue;
            }
            c->probes++;
            c->timer = now;
            send_control(a, i, ADSP_CONTROL | ADSP_ACK_REQ | ADSP_PROBE_OR_ACK);
        }
    }
}

int adsp_is_open(const adsp_t *a, int i)
{
    const adsp_conn_t *c = conn(a, i);
    return c && c->state == ST_OPEN;
}

uint8_t adsp_sock(const adsp_t *a, int i)
{
    const adsp_conn_t *c = conn(a, i);
    return c && c->state != ST_FREE ? c->local_sock : 0;
}

const lt_route_t *adsp_peer(const adsp_t *a, int i)
{
    const adsp_conn_t *c = conn(a, i);
    return c && c->state != ST_FREE ? &c->peer : NULL;
}
