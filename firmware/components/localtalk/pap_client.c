// SPDX-License-Identifier: GPL-3.0-or-later
// The PAP client (W-1082); see pap_client.h.
#include "pap_client.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void say(pap_client_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(pap_client_t *c, const char *fmt, ...)
{
    if (!c->ops.log) return;
    char line[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    c->ops.log(c->ops.ctx, line);
}

// How long since `then`; 0 for a stamp later than `now` (a frame handled at
// a later clock than the tick that follows it), never a wrapped huge one.
static uint64_t since(uint64_t now, uint64_t then)
{
    return now > then ? now - then : 0;
}

static uint32_t rnd(pap_client_t *c)
{
    c->rng ^= c->rng << 13;
    c->rng ^= c->rng >> 17;
    c->rng ^= c->rng << 5;
    return c->rng;
}

static void send(pap_client_t *c, const lt_route_t *to, uint8_t sock, uint8_t ctl, uint8_t bitmap,
                 uint16_t tid, uint8_t u1, uint8_t u2, uint8_t u3, const uint8_t *d, size_t n)
{
    uint8_t user[4] = { c->connid, u1, u2, u3 };
    if (u1 == PAP_SEND_STATUS) user[0] = 0;
    lt_node_send_atp(c->node, to, sock, ctl, bitmap, tid, user, d, n);
}

static void release(pap_client_t *c, const lt_route_t *to, uint16_t tid)
{
    static const uint8_t none[4] = { 0 };
    lt_node_send_atp(c->node, to, PAPC_SOCK, ATP_TREL, 0, tid, none, NULL, 0);
}

static void finish(pap_client_t *c, papc_end_t how)
{
    if (c->phase == PAPC_IDLE) return;
    static const char *const names[] = { "printed", "canceled", "busy too long", "no answer",
                                         "refused", "dropped" };
    say(c, "PAP %u to %u: %s, %lu bytes sent", c->connid, c->sls.addr.node,
        names[how], (unsigned long)c->sent_bytes);
    c->phase = PAPC_IDLE;
    if (c->ops.ended) c->ops.ended(c->ops.ctx, how);
}

void papc_init(pap_client_t *c, const papc_ops_t *ops, uint32_t seed)
{
    lt_node_t *node = c->node;
    memset(c, 0, sizeof *c);
    c->node = node;
    c->ops = *ops;
    c->dead_ms = PAPC_DEAD_MS;
    c->rng = seed ? seed : 0x1082u;
}

void papc_set_dead_ms(pap_client_t *c, uint32_t ms) { c->dead_ms = ms; }

int papc_idle(const pap_client_t *c) { return c->phase == PAPC_IDLE; }

// ------------------------------------------------------------- opening

static void send_open(pap_client_t *c, uint64_t now)
{
    uint32_t waited = (uint32_t)((since(now, c->first_try)) / 1000);
    uint8_t data[4] = { PAPC_SOCK, PAPC_FLOW, (uint8_t)(waited >> 8), (uint8_t)waited };
    send(c, &c->sls, PAPC_SOCK, ATP_TREQ | ATP_XO, 0x01, c->open_tid, PAP_OPEN_CONN, 0, 0,
         data, sizeof data);
    c->open_out = 1;
    c->open_at = now;
}

int papc_open(pap_client_t *c, const lt_route_t *to, uint32_t busy_ms, uint64_t now)
{
    if (!c->node || c->phase != PAPC_IDLE) return -1;
    lt_node_t *node = c->node;
    papc_ops_t ops = c->ops;
    uint32_t dead = c->dead_ms, rng = c->rng;
    // A fresh connection: nothing of the last one carries over but the
    // callbacks, the timers' settings and the random state.
    lt_route_t st_to = c->st_to;
    uint16_t st_tid = c->st_tid;
    uint8_t st_tries = c->st_tries;
    uint64_t st_at = c->st_at;
    memset(c, 0, sizeof *c);
    c->node = node; c->ops = ops; c->dead_ms = dead; c->rng = rng;
    c->st_to = st_to; c->st_tid = st_tid; c->st_tries = st_tries; c->st_at = st_at;

    c->sls = *to;
    c->busy_ms = busy_ms;
    do c->connid = (uint8_t)rnd(c); while (!c->connid);
    c->phase = PAPC_OPENING;
    c->first_try = c->answered = now;
    c->open_tid = lt_node_next_tid(c->node);
    say(c, "PAP %u: opening a connection to %u:%u", c->connid, to->addr.node, to->addr.sock);
    send_open(c, now);
    return 0;
}

static void start_read(pap_client_t *c, uint64_t now);

static void open_reply(pap_client_t *c, const atp_t *a, uint64_t now)
{
    if (!c->open_out || a->tid != c->open_tid || a->len < 4) return;
    c->open_out = 0;
    c->answered = now;
    release(c, &c->sls, a->tid);
    uint16_t result = (uint16_t)(a->data[2] << 8 | a->data[3]);
    const uint8_t *st = a->len > 4 && 5 + (size_t)a->data[4] <= a->len ? a->data + 4 : NULL;
    if (result == PAP_RESULT_OK) {
        c->connid = a->user[0] ? a->user[0] : c->connid;
        c->sess = c->sls;
        c->sess.addr.sock = a->data[0];
        c->quantum = a->data[1] >= 1 && a->data[1] <= PAPC_FLOW ? a->data[1] : 1;
        c->phase = PAPC_OPEN;
        c->heard = c->tickled = now;
        c->rd_seq = 1;
        say(c, "PAP %u: open, the printer's socket %u, quantum %u", c->connid,
            c->sess.addr.sock, c->quantum);
        if (c->ops.opened) c->ops.opened(c->ops.ctx);
        start_read(c, now);
        return;
    }
    if (st && c->ops.status) c->ops.status(c->ops.ctx, st);
    if (result != PAP_RESULT_BUSY) {
        say(c, "PAP %u: refused (result %04x)", c->connid, result);
        finish(c, PAPC_END_REFUSED);
        return;
    }
    if (since(now, c->first_try) >= c->busy_ms) {
        finish(c, PAPC_END_BUSY);
        return;
    }
    c->next_open = c->open_at + PAPC_BUSY_RETRY_MS;   // every 2 s from the last try
}

static void tick_opening(pap_client_t *c, uint64_t now)
{
    if (since(now, c->answered) >= PAPC_GONE_MS) {
        finish(c, PAPC_END_DEAD);
        return;
    }
    if (since(now, c->first_try) >= c->busy_ms) {
        finish(c, PAPC_END_BUSY);
        return;
    }
    if (c->open_out) {
        if (since(now, c->open_at) >= PAPC_RETRY_MS) send_open(c, now);    // the same TID: XO
        return;
    }
    if (now >= c->next_open) {
        c->open_tid = lt_node_next_tid(c->node);
        send_open(c, now);
    }
}

// ------------------------------------------------------------- our data

static void send_reply(pap_client_t *c, const papc_reply_t *r, uint16_t tid, const lt_route_t *to)
{
    for (int k = 0; k < r->n; k++) {
        uint8_t ctl = ATP_TRESP | (k == r->n - 1 ? ATP_EOM : 0);
        send(c, to, PAPC_SOCK, ctl, (uint8_t)k, tid, PAP_DATA, r->eof, 0, r->pkt[k], r->len[k]);
    }
}

static papc_reply_t *cached(pap_client_t *c, uint16_t seq)
{
    if (!seq) return NULL;                       // 0: unsequenced, never a retransmit
    for (int i = 0; i < PAPC_CACHE; i++)
        if (c->cache[i].used && c->cache[i].seq == seq) return &c->cache[i];
    return NULL;
}

static void top_up(pap_client_t *c)
{
    while (!c->src_eof && !c->cancel && c->staged < sizeof c->stage) {
        int eof = 0;
        size_t n = c->ops.read ? c->ops.read(c->ops.ctx, c->stage + c->staged,
                                             sizeof c->stage - c->staged, &eof) : 0;
        c->staged += n;
        if (eof || !c->ops.read) c->src_eof = 1;
        if (!n) break;
    }
}

// Answers the oldest pending SendData once there is a full answer, the
// job's end, or it has waited PAPC_HOLD_MS with something to send.
static int answer_one(pap_client_t *c, papc_pend_t *p, uint64_t now)
{
    size_t room = (size_t)p->want * PAP_QUANTUM;
    int done = c->cancel || c->eof_sent || c->src_eof;
    if (!done && c->staged < room && !(c->staged && since(now, p->since) >= PAPC_HOLD_MS)) return 0;

    papc_reply_t *r = &c->cache[c->cache_next];
    c->cache_next = (uint8_t)((c->cache_next + 1) % PAPC_CACHE);
    memset(r, 0, sizeof *r);
    r->used = 1;
    r->seq = p->seq;
    size_t take = c->cancel || c->eof_sent ? 0 : c->staged < room ? c->staged : room;
    r->eof = c->cancel || c->eof_sent || (c->src_eof && take == c->staged);
    size_t off = 0;
    do {                                         // an empty answer is one empty packet
        size_t k = take - off < PAP_QUANTUM ? take - off : PAP_QUANTUM;
        memcpy(r->pkt[r->n], c->stage + off, k);
        r->len[r->n++] = (uint16_t)k;
        off += k;
    } while (off < take);
    memmove(c->stage, c->stage + take, c->staged - take);
    c->staged -= take;
    c->sent_bytes += (uint32_t)take;
    send_reply(c, r, p->tid, &p->from);
    p->used = 0;
    if (r->eof && !c->eof_sent) {
        c->eof_sent = 1;
        c->eof_at = now;
        c->phase = PAPC_DRAINING;
        say(c, "PAP %u: EOF sent after %lu bytes", c->connid, (unsigned long)c->sent_bytes);
    }
    return 1;
}

static papc_pend_t *oldest(pap_client_t *c)
{
    papc_pend_t *o = NULL;
    for (int i = 0; i < PAPC_CACHE; i++)
        if (c->pend[i].used && (!o || c->pend[i].since < o->since)) o = &c->pend[i];
    return o;
}

static void answer_pending(pap_client_t *c, uint64_t now)
{
    papc_pend_t *p;
    while ((p = oldest(c))) {
        top_up(c);
        if (!answer_one(c, p, now)) break;
    }
}

static void printer_reads(pap_client_t *c, const atp_t *a, const lt_route_t *from, uint64_t now)
{
    uint16_t seq = (uint16_t)(a->user[2] << 8 | a->user[3]);
    papc_reply_t *r = cached(c, seq);
    if (r) {                                     // a retransmit: the same packets
        send_reply(c, r, a->tid, from);
        return;
    }
    papc_pend_t *slot = NULL;
    for (int i = 0; i < PAPC_CACHE; i++)
        if (c->pend[i].used && seq && c->pend[i].seq == seq) slot = &c->pend[i];
    if (slot) {                                  // already waiting: answer the latest TID
        slot->tid = a->tid;
        slot->from = *from;
        return;
    }
    for (int i = 0; !slot && i < PAPC_CACHE; i++)
        if (!c->pend[i].used) slot = &c->pend[i];
    if (!slot) return;                           // more than the quantum allows: dropped
    uint8_t want = 0;
    while (want < PAPC_FLOW && (a->bitmap >> want & 1)) want++;
    slot->used = 1;
    slot->want = want ? want : 1;
    slot->seq = seq;
    slot->tid = a->tid;
    slot->from = *from;
    slot->since = now;
    answer_pending(c, now);
}

// ------------------------------------------------------------- the printer's output

static void send_read(pap_client_t *c, uint64_t now)
{
    send(c, &c->sess, PAPC_SOCK, ATP_TREQ | ATP_XO, c->rd_want, c->rd_tid, PAP_SEND_DATA,
         (uint8_t)(c->rd_seq >> 8), (uint8_t)c->rd_seq, NULL, 0);
    c->rd_sent = now;
}

static void start_read(pap_client_t *c, uint64_t now)
{
    if (c->rd_want || c->eof_got) return;
    c->rd_tid = lt_node_next_tid(c->node);
    c->rd_want = (uint8_t)((1u << c->quantum) - 1);
    memset(c->rd_len, 0, sizeof c->rd_len);
    c->rd_eof = 0;
    send_read(c, now);
}

static void send_close(pap_client_t *c, uint64_t now)
{
    send(c, &c->sess, PAPC_SOCK, ATP_TREQ | ATP_XO, 0x01, c->close_tid, PAP_CLOSE_CONN, 0, 0,
         NULL, 0);
    c->close_tries++;
    c->close_at = now;
}

static void close_conn(pap_client_t *c, papc_end_t how, uint64_t now)
{
    c->end = how;
    c->phase = PAPC_CLOSING;
    c->close_tid = lt_node_next_tid(c->node);
    c->close_tries = 0;
    send_close(c, now);
}

static void read_done(pap_client_t *c, uint64_t now)
{
    release(c, &c->sess, c->rd_tid);
    for (int k = 0; k < PAPC_FLOW; k++) {
        if (!c->rd_len[k]) continue;
        c->out_bytes += c->rd_len[k];
        if (c->ops.output) c->ops.output(c->ops.ctx, c->rd_buf[k], c->rd_len[k]);
    }
    c->rd_want = 0;
    c->rd_seq = c->rd_seq == 0xFFFF ? 1 : (uint16_t)(c->rd_seq + 1);
    if (c->rd_eof) {
        c->eof_got = 1;
        say(c, "PAP %u: the printer's EOF after %lu bytes of output", c->connid,
            (unsigned long)c->out_bytes);
        if (c->eof_sent) close_conn(c, c->cancel ? PAPC_END_CANCELED : PAPC_END_OK, now);
        return;
    }
    start_read(c, now);
}

static void printer_data(pap_client_t *c, const atp_t *a, uint64_t now)
{
    uint8_t seq = a->bitmap;
    if (!c->rd_want || seq >= PAPC_FLOW || !(c->rd_want & (1u << seq))) return;
    size_t n = a->len < PAP_QUANTUM ? a->len : PAP_QUANTUM;
    memcpy(c->rd_buf[seq], a->data, n);
    c->rd_len[seq] = (uint16_t)n;
    c->rd_want &= (uint8_t)~(1u << seq);
    if (a->user[2]) c->rd_eof = 1;
    if (a->ctl & ATP_EOM) c->rd_want &= (uint8_t)((1u << (seq + 1)) - 1);
    if (!c->rd_want) read_done(c, now);
}

// ------------------------------------------------------------- frames in

void papc_rx(pap_client_t *c, const ddp_t *d, const atp_t *a, const lt_route_t *from,
             uint64_t now)
{
    uint8_t fn = a->ctl & ATP_FN_MASK;
    if (d->dst.sock == PAPC_SOCK_ST) {
        if (fn != ATP_TRESP || !c->st_tries || a->tid != c->st_tid || a->user[1] != PAP_STATUS)
            return;
        c->st_tries = 0;
        if (a->len > 4 && 5 + (size_t)a->data[4] <= a->len && c->ops.status)
            c->ops.status(c->ops.ctx, a->data + 4);
        return;
    }
    if (c->phase == PAPC_IDLE) return;
    if (d->src.node != c->sls.addr.node) return;
    if (c->phase == PAPC_OPENING) {
        if (fn == ATP_TRESP && a->user[1] == PAP_OPEN_CONN_REPLY) open_reply(c, a, now);
        return;
    }
    if (a->user[0] != c->connid) return;
    c->heard = now;
    if (fn == ATP_TRESP) {
        if (a->tid == c->rd_tid && a->user[1] == PAP_DATA) printer_data(c, a, now);
        else if (c->phase == PAPC_CLOSING && a->tid == c->close_tid &&
                 a->user[1] == PAP_CLOSE_CONN_REPLY) {
            release(c, &c->sess, a->tid);
            finish(c, c->end);
        }
        return;
    }
    if (fn != ATP_TREQ) return;
    switch (a->user[1]) {
    case PAP_SEND_DATA:
        if (c->phase != PAPC_CLOSING) printer_reads(c, a, from, now);
        break;
    case PAP_CLOSE_CONN: {
        uint8_t user[4] = { c->connid, PAP_CLOSE_CONN_REPLY, 0, 0 };
        lt_node_send_atp(c->node, from, PAPC_SOCK, ATP_TRESP | ATP_EOM, 0, a->tid, user, NULL, 0);
        say(c, "PAP %u: closed by the printer", c->connid);
        finish(c, c->phase == PAPC_CLOSING ? c->end
                  : c->eof_sent ? (c->cancel ? PAPC_END_CANCELED : PAPC_END_OK)
                  : PAPC_END_DROPPED);
        break;
    }
    default:                                     // a tickle: heard, nothing to answer
        break;
    }
}

// ------------------------------------------------------------- the clock

static void tick_status(pap_client_t *c, uint64_t now)
{
    if (!c->st_tries || since(now, c->st_at) < PAPC_RETRY_MS) return;
    if (c->st_tries >= PAPC_TRIES) {
        c->st_tries = 0;                         // unanswered: the caller's timer decides
        return;
    }
    send(c, &c->st_to, PAPC_SOCK_ST, ATP_TREQ, 0x01, c->st_tid, PAP_SEND_STATUS, 0, 0, NULL, 0);
    c->st_tries++;
    c->st_at = now;
}

void papc_tick(pap_client_t *c, uint64_t now)
{
    tick_status(c, now);
    switch (c->phase) {
    case PAPC_OPENING:
        tick_opening(c, now);
        return;
    case PAPC_CLOSING:
        if (since(now, c->close_at) < PAPC_RETRY_MS) return;
        if (c->close_tries >= PAPC_TRIES) finish(c, c->end);
        else send_close(c, now);
        return;
    case PAPC_OPEN:
    case PAPC_DRAINING:
        break;
    default:
        return;
    }
    if (since(now, c->heard) >= c->dead_ms) {
        say(c, "PAP %u: the printer went silent", c->connid);
        finish(c, PAPC_END_DEAD);
        return;
    }
    if (since(now, c->tickled) >= PAPC_TICKLE_MS) {
        send(c, &c->sess, PAPC_SOCK, ATP_TREQ, 0, lt_node_next_tid(c->node), PAP_TICKLE, 0, 0,
             NULL, 0);
        c->tickled = now;
    }
    answer_pending(c, now);
    if (c->phase != PAPC_OPEN && c->phase != PAPC_DRAINING) return;
    if (c->rd_want && since(now, c->rd_sent) >= PAPC_READ_RETRY_MS) send_read(c, now);
    start_read(c, now);
    if (c->phase == PAPC_DRAINING && since(now, c->eof_at) >= PAPC_EOF_WAIT_MS) {
        say(c, "PAP %u: no EOF from the printer %u s after ours; closing", c->connid,
            PAPC_EOF_WAIT_MS / 1000);
        close_conn(c, c->cancel ? PAPC_END_CANCELED : PAPC_END_OK, now);
    }
}

// ------------------------------------------------------------- the rest

void papc_cancel(pap_client_t *c, uint64_t now)
{
    if (c->phase == PAPC_IDLE) return;
    if (c->phase == PAPC_OPENING) {
        finish(c, PAPC_END_CANCELED);
        return;
    }
    if (!c->cancel) say(c, "PAP %u: canceled", c->connid);
    c->cancel = 1;
    answer_pending(c, now);
}

void papc_status(pap_client_t *c, const lt_route_t *to, uint64_t now)
{
    if (!c->node) return;
    c->st_to = *to;
    c->st_tid = lt_node_next_tid(c->node);
    c->st_tries = 0;
    c->st_at = now;
    send(c, &c->st_to, PAPC_SOCK_ST, ATP_TREQ, 0x01, c->st_tid, PAP_SEND_STATUS, 0, 0, NULL, 0);
    c->st_tries = 1;
}

void papc_link_down(pap_client_t *c, uint64_t now)
{
    (void)now;
    c->st_tries = 0;
    finish(c, PAPC_END_DROPPED);
}
