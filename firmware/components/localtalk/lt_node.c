// SPDX-License-Identifier: GPL-3.0-or-later
// The LocalTalk node; see lt_node.h.
#include "lt_node.h"
#include "pap_client.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void say(lt_node_t *n, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(lt_node_t *n, const char *fmt, ...)
{
    char line[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n->ops.log) n->ops.log(n->ops.ctx, line);
}

// A Pascal name for a log line: non-printables as '.'.
static const char *show(const uint8_t *p, char *buf)
{
    size_t k = p[0];
    for (size_t i = 0; i < k; i++) buf[i] = (p[1 + i] >= 0x20 && p[1 + i] < 0x7F) ? (char)p[1 + i] : '.';
    buf[k] = '\0';
    return buf;
}

static uint32_t rnd(lt_node_t *n)
{
    uint32_t x = n->rng;            // xorshift32
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return n->rng = x;
}

static uint8_t new_candidate(lt_node_t *n)
{
    uint8_t id;
    do id = (uint8_t)(128 + rnd(n) % 127); while (id == n->node);   // 128..254
    return id;
}

static void send_ddp(lt_node_t *n, const lt_route_t *r, uint8_t src_sock, uint8_t type,
                     const uint8_t *data, size_t len)
{
    uint8_t f[LLAP_MAX];
    lt_addr_t me = { n->net, n->node, src_sock };
    size_t fl = ddp_build(f, sizeof f, r->via, n->node, r->is_long, &r->addr, &me, type, data, len);
    if (fl) n->ops.send(n->ops.ctx, f, fl);
}

static void send_atp(lt_node_t *n, const lt_route_t *r, uint8_t src_sock, uint8_t ctl,
                     uint8_t bitmap, uint16_t tid, const uint8_t user[4],
                     const uint8_t *data, size_t len)
{
    uint8_t a[ATP_HDR + ATP_MAX_DATA];
    size_t al = atp_build(a, sizeof a, ctl, bitmap, tid, user, data, len);
    if (al) send_ddp(n, r, src_sock, DDP_ATP, a, al);
}

// ------------------------------------------------------------- XO cache

static int same_route(const lt_route_t *a, const lt_route_t *b)
{
    return a->addr.net == b->addr.net && a->addr.node == b->addr.node &&
           a->addr.sock == b->addr.sock;
}

static lt_xo_t *xo_find(lt_node_t *n, const lt_route_t *from, uint16_t tid)
{
    for (int i = 0; i < LT_XO_SLOTS; i++)
        if (n->xo[i].used && n->xo[i].tid == tid && same_route(&n->xo[i].to, from))
            return &n->xo[i];
    return NULL;
}

// Send a one-packet TResp; when the request was XO, keep it for duplicates
// until the requester's TRel or the release timer.
static void respond(lt_node_t *n, const lt_route_t *to, uint8_t src_sock, const atp_t *req,
                    const uint8_t user[4], const uint8_t *data, size_t len, uint64_t now)
{
    uint8_t a[LT_XO_PKT];
    size_t al = atp_build(a, sizeof a, ATP_TRESP | ATP_EOM, 0, req->tid, user, data, len);
    if (!al) return;
    send_ddp(n, to, src_sock, DDP_ATP, a, al);
    if (!(req->ctl & ATP_XO)) return;

    lt_xo_t *x = xo_find(n, to, req->tid);
    for (int i = 0; !x && i < LT_XO_SLOTS; i++)
        if (!n->xo[i].used || n->xo[i].expires <= now) x = &n->xo[i];
    if (!x) {                                  // all live: evict the soonest to expire
        x = &n->xo[0];
        for (int i = 1; i < LT_XO_SLOTS; i++)
            if (n->xo[i].expires < x->expires) x = &n->xo[i];
    }
    unsigned ind = req->ctl & ATP_TREL_MASK;
    x->used = 1;
    x->src_sock = src_sock;
    x->to = *to;
    x->tid = req->tid;
    x->expires = now + 30000ull * (1u << (ind <= 4 ? ind : 0));
    x->len = (uint16_t)al;
    memcpy(x->atp, a, al);
}

// ------------------------------------------------------------- NBP

static void name_reply(lt_node_t *n, const ddp_t *d, const nbp_t *q, uint64_t now);
static void name_registering(lt_node_t *n, const ddp_t *d, const nbp_t *q, uint64_t now);
static void printer_note(lt_node_t *n, const ddp_t *d, const lt_addr_t *a);

static void on_nbp(lt_node_t *n, const ddp_t *d, uint64_t now)
{
    nbp_t q;
    if (d->dst.sock != SOCK_NIS || nbp_parse(d->data, d->len, &q)) return;
    if (q.fn == NBP_LKUP_REPLY) {
        uint8_t ws[NBP_NAME_MAX + 1];
        nbp_name(ws, LT_NAME_TYPE);
        if (nbp_match(ws, q.t.type)) {
            name_reply(n, d, &q, now);
        } else if (q.count && q.t.obj[0]) {
            // The node asks for nothing else: an answer to lt_node_lookup.
            printer_note(n, d, &q.t.addr);
            if (!n->ops.found) return;
            // W-1082: reach its socket as on_nbp answers a lookup: through
            // the router that relayed it, if any.
            lt_route_t r = { q.t.addr, q.t.addr.node, 0 };
            if (!r.addr.net) r.addr.net = d->src.net;
            if (d->is_long || (q.t.addr.net && q.t.addr.net != n->net)) {
                r.via = d->llap_src;
                r.is_long = 1;
            }
            n->ops.found(n->ops.ctx, &q.t, &r);
        }
        return;
    }
    if (q.fn != NBP_LKUP) return;
    n->st.lookups++;
    name_registering(n, d, &q, now);
    // The node's own name while it serves, and each registration (W-1088).
    int hits[LT_REG_MAX] = { 0 }, hit = 0;
    hits[0] = n->serve && n->obj[0] && nbp_match(q.t.obj, n->obj) && nbp_match(q.t.type, n->type);
    for (int r = 1; r < LT_REG_MAX && n->reg; r++)
        hits[r] = n->reg[r].sls && nbp_match(q.t.obj, n->reg[r].obj) &&
                  nbp_match(q.t.type, n->reg[r].type);
    for (int r = 0; r < LT_REG_MAX; r++) hit |= hits[r];

    // Log a lookup once, not every second the Chooser repeats it.
    uint8_t key[sizeof n->last_lkup];
    size_t k = 0;
    key[k++] = d->src.node;
    memcpy(key + k, q.t.obj, 1 + (size_t)q.t.obj[0]);   k += 1 + q.t.obj[0];
    memcpy(key + k, q.t.type, 1 + (size_t)q.t.type[0]); k += 1 + q.t.type[0];
    memcpy(key + k, q.t.zone, 1 + (size_t)q.t.zone[0]); k += 1 + q.t.zone[0];
    memset(key + k, 0, sizeof key - k);
    if (memcmp(key, n->last_lkup, sizeof key) || now - n->last_lkup_at > 30000) {
        char o[NBP_NAME_MAX + 1], t[NBP_NAME_MAX + 1], z[NBP_NAME_MAX + 1];
        say(n, "LkUp %s:%s@%s from %u:%u -> %s", show(q.t.obj, o), show(q.t.type, t),
            show(q.t.zone, z), d->src.node, q.t.addr.sock, hit ? "answered" : "not us");
        memcpy(n->last_lkup, key, sizeof key);
        n->last_lkup_at = now;
    }
    if (!hit) return;

    // Reply to the socket in the tuple. One on another network (a router
    // relayed the lookup) gets a long header through the router that sent it.
    lt_route_t to = { q.t.addr, q.t.addr.node, 0 };
    if (d->is_long || (q.t.addr.net && q.t.addr.net != n->net)) {
        to.via = d->llap_src;
        to.is_long = 1;
    }
    for (int r = 0; r < LT_REG_MAX; r++) {
        if (!hits[r]) continue;
        nbp_tuple_t me = { .addr = { n->net, n->node, r ? n->reg[r].sls : LT_SOCK_SLS },
                           .enumerator = (uint8_t)r };
        memcpy(me.obj, r ? n->reg[r].obj : n->obj, sizeof me.obj);
        memcpy(me.type, r ? n->reg[r].type : n->type, sizeof me.type);
        me.zone[0] = 1;
        me.zone[1] = '*';
        uint8_t pkt[2 + 5 + 3 * (NBP_NAME_MAX + 1)];
        size_t pl = nbp_build(pkt, sizeof pkt, NBP_LKUP_REPLY, q.id, &me);
        if (pl) {
            send_ddp(n, &to, SOCK_NIS, DDP_NBP, pkt, pl);
            n->st.replies++;
        }
    }
}

// ------------------------------------------------------------- PAP

static uint16_t next_tid(lt_node_t *n) { return n->tid++; }

// The status as the card sends it: a Pascal string of the word, low byte first.
static void status_bytes(const lt_node_t *n, uint8_t out[3])
{
    uint16_t w = n->status;
    out[0] = 2;
    out[1] = (uint8_t)w;
    out[2] = (uint8_t)(w >> 8);
}

static void job_end(lt_node_t *n, int normal)
{
    if (n->pap.ended) return;
    n->pap.ended = 1;
    if (n->ops.job_end) n->ops.job_end(n->ops.ctx, normal);
}

static void pap_close(lt_node_t *n, const char *why, int normal)
{
    say(n, "PAP %u closed (%s) after %lu bytes", n->pap.connid, why, (unsigned long)n->pap.bytes);
    job_end(n, normal);
    n->pap.open = 0;
}

static size_t job_room(lt_node_t *n)
{
    return n->ops.job_room ? n->ops.job_room(n->ops.ctx) : (size_t)-1;
}

static void pap_send_read(lt_node_t *n, uint64_t now)
{
    lt_pap_t *p = &n->pap;
    uint8_t user[4] = { p->connid, PAP_SEND_DATA, (uint8_t)(p->seq >> 8), (uint8_t)p->seq };
    send_atp(n, &p->ws, LT_SOCK_CONN, ATP_TREQ | ATP_XO, p->rd_want, p->rd_tid, user, NULL, 0);
    p->rd_sent = now;
}

static void pap_start_read(lt_node_t *n, uint64_t now)
{
    lt_pap_t *p = &n->pap;
    // Back-pressure: a read can bring LT_READ_MAX bytes, so it waits for that
    // much room. A paused printer holds the Mac here, not in our buffers.
    if (!p->open || p->done || p->rd_want || job_room(n) < LT_READ_MAX) return;
    p->rd_tid = next_tid(n);
    p->rd_want = (uint8_t)((1u << LT_FLOW) - 1);
    memset(p->rd_len, 0, sizeof p->rd_len);
    p->rd_eof = 0;
    pap_send_read(n, now);
}

// Answer the workstation's own SendData (it reads us back). We have nothing
// to say -- the Option Card has no reverse channel -- so the answer is empty:
// EOF once our side has seen the job's EOF, otherwise a keep-alive once the
// workstation has been quiet LT_PAP_READ_IDLE_MS.
static void pap_reply_ws(lt_node_t *n, uint16_t tid, const lt_route_t *to, uint8_t eof,
                         const uint8_t *data, size_t len, uint64_t now)
{
    lt_pap_t *p = &n->pap;
    uint8_t user[4] = { p->connid, PAP_DATA, eof, 0 };
    atp_t req = { .ctl = ATP_TREQ | ATP_XO, .tid = tid };
    respond(n, to, LT_SOCK_CONN, &req, user, data, len, now);
    n->st.ws_answers++;
}

// W-1088: a relayed connection has the printer's own output to give.
static int relayed(const lt_node_t *n)
{
    return n->pap.reg && n->ops.reg_output;
}

static void pap_answer_ws_read(lt_node_t *n, uint64_t now)
{
    lt_pap_t *p = &n->pap;
    if (!p->ws_read) return;
    if (relayed(n)) {
        uint8_t out[LT_RELAY_PKT];
        int end = 0;
        size_t k = p->eof_sent ? 0 : n->ops.reg_output(n->ops.ctx, p->reg, out, sizeof out, &end, 0);
        // Held for the printer's words; a keep-alive once the Mac has been quiet.
        if (!k && !end && now - p->wrote < LT_PAP_READ_IDLE_MS) return;
        pap_reply_ws(n, p->ws_read_tid, &p->ws_read_from, (uint8_t)end, out, k, now);
        if (end) {
            p->eof_sent = 1;
            say(n, "PAP %u: the printer's EOF to the Mac's read", p->connid);
        }
        p->ws_answered_seq = p->ws_read_seq;
        p->ws_answered_eof = (uint8_t)end;
        p->ws_read = 0;
        return;
    }
    uint8_t eof = p->done && !p->eof_sent;
    if (!eof && now - p->wrote < LT_PAP_READ_IDLE_MS) return;
    pap_reply_ws(n, p->ws_read_tid, &p->ws_read_from, eof, NULL, 0, now);
    if (eof) {
        p->eof_sent = 1;
        say(n, "PAP %u: EOF to the Mac's read", p->connid);
    }
    p->ws_answered_seq = p->ws_read_seq;
    p->ws_answered_eof = eof;
    p->ws_read = 0;
}

static void pap_read_done(lt_node_t *n, uint64_t now)
{
    lt_pap_t *p = &n->pap;
    uint8_t none[4] = {0};
    send_atp(n, &p->ws, LT_SOCK_CONN, ATP_TREL, 0, p->rd_tid, none, NULL, 0);

    size_t got = 0;
    for (int i = 0; i < LT_FLOW; i++) got += p->rd_len[i];
    if (p->bytes == 0 && got) {
        // The first bytes of a job are what tells us what PAP delivers.
        char hex[3 * 24 + 1] = {0};
        size_t show_n = p->rd_len[0] < 24 ? p->rd_len[0] : 24;
        for (size_t i = 0; i < show_n; i++) snprintf(hex + 3 * i, 4, "%02x ", p->rd_buf[0][i]);
        say(n, "PAP %u first bytes: %s", p->connid, hex);
    }
    for (int i = 0; i < LT_FLOW; i++) {
        if (!p->rd_len[i]) continue;
        if (n->ops.job_data) n->ops.job_data(n->ops.ctx, p->rd_buf[i], p->rd_len[i]);
    }
    p->bytes += (uint32_t)got;
    p->rd_want = 0;
    if (p->rd_eof) {
        job_end(n, 1);
        p->done = 1;
        say(n, "PAP %u EOF after %lu bytes", p->connid, (unsigned long)p->bytes);
        pap_answer_ws_read(n, now);
        return;
    }
    p->seq = p->seq == 0xFFFF ? 1 : (uint16_t)(p->seq + 1);
    pap_start_read(n, now);
}

static void pap_open(lt_node_t *n, const ddp_t *d, const atp_t *a, const lt_route_t *from,
                     uint64_t now, int reg, uint8_t sls)
{
    lt_pap_t *p = &n->pap;
    if (a->len < 4) return;
    lt_route_t ws = *from;
    ws.addr.sock = a->data[0];                 // the workstation's ATP responding socket

    int same_conn = p->open && p->connid == a->user[0] && same_route(&p->ws, &ws);
    if (p->open && !same_conn && ws.addr.node == p->ws.addr.node && ws.addr.net == p->ws.addr.net &&
        now - p->wrote >= LT_PAP_STALE_MS)
        pap_close(n, "the Mac opened another", 0);
    uint16_t result = (p->open && !same_conn) ? PAP_RESULT_BUSY : PAP_RESULT_OK;
    // The printer may be someone else's: an IPP job, a test page, or the
    // last LocalTalk job still draining.
    int printer_busy = 0;
    if (result == PAP_RESULT_OK && !same_conn) {
        // W-1088: a registration's printer is its owner's to give.
        int busy = reg ? (!n->ops.pap_open || n->ops.pap_open(n->ops.ctx, reg, now))
                       : (n->ops.job_begin && n->ops.job_begin(n->ops.ctx));
        if (busy) {
            result = PAP_RESULT_BUSY;
            printer_busy = 1;
        }
    }

    uint8_t data[4 + 256];
    data[0] = result == PAP_RESULT_OK ? LT_SOCK_CONN : 0;
    data[1] = LT_FLOW;
    data[2] = (uint8_t)(result >> 8);
    data[3] = (uint8_t)result;
    // A registration's printer speaks for itself (W-1088): its own status words.
    size_t sl = reg && n->ops.reg_status ? n->ops.reg_status(n->ops.ctx, reg, data + 4, 256) : 0;
    if (!sl) {
        status_bytes(n, data + 4);
        sl = 3;
    }
    uint8_t user[4] = { a->user[0], PAP_OPEN_CONN_REPLY, 0, 0 };
    respond(n, from, sls, a, user, data, 4 + sl, now);

    if (result == PAP_RESULT_BUSY) {
        if (n->st.busy++ == 0 || !(n->st.busy % 10)) {
            if (printer_busy)
                say(n, "PAP OpenConn from %u while the printer is busy (%lu so far)", d->src.node,
                    (unsigned long)n->st.busy);
            else
                say(n, "PAP OpenConn from %u while %u is open (%lu so far)", d->src.node,
                    p->connid, (unsigned long)n->st.busy);
        }
        return;
    }
    if (same_conn) return;
    memset(p, 0, sizeof *p);
    p->open = 1;
    p->reg = (uint8_t)reg;
    p->connid = a->user[0];
    p->ws = ws;
    p->heard = p->tickled = p->wrote = p->idle_since = now;
    p->tickle_tid = next_tid(n);
    p->seq = 1;
    n->st.opens++;
    n->st.jobs++;
    say(n, "PAP %u open from %u:%u, its quantum %u", p->connid, d->src.node, ws.addr.sock,
        a->data[1]);
    pap_start_read(n, now);
}

static void on_atp(lt_node_t *n, const ddp_t *d, uint64_t now)
{
    atp_t a;
    if (atp_parse(d->data, d->len, &a)) return;
    lt_route_t from = { d->src, d->llap_src, d->is_long };
    uint8_t fn = a.ctl & ATP_FN_MASK;
    lt_pap_t *p = &n->pap;

    // W-1082: the PAP client's sockets are its own, retransmits included.
    if (d->dst.sock == LT_SOCK_PAPC || d->dst.sock == LT_SOCK_PAPC_ST) {
        if (n->client) papc_rx(n->client, d, &a, &from, now);
        return;
    }

    if (fn == ATP_TREL) {
        lt_xo_t *x = xo_find(n, &from, a.tid);
        if (x) x->used = 0;
        return;
    }
    if (fn == ATP_TREQ && (a.ctl & ATP_XO)) {
        lt_xo_t *x = xo_find(n, &from, a.tid);
        if (x && x->expires > now) {               // a duplicate: say it again
            send_ddp(n, &from, x->src_sock, DDP_ATP, x->atp, x->len);
            if (p->open && same_route(&p->ws, &from)) p->heard = now;
            return;
        }
    }

    int reg = -1;
    // W-1082, W-1088: no ImageWriter here, or none on the port to offer.
    if (d->dst.sock == LT_SOCK_SLS) reg = n->serve && n->obj[0] ? 0 : -1;
    else for (int r = 1; r < LT_REG_MAX && n->reg; r++) if (n->reg[r].sls && n->reg[r].sls == d->dst.sock) reg = r;
    if (reg >= 0 && fn == ATP_TREQ) {
        uint8_t aw[NBP_NAME_MAX + 1];
        nbp_name(aw, LT_AIRWRITER_TYPE);
        if (reg && nbp_match(aw, n->reg[reg].type)) return;   // the marker never opens
        uint8_t sls = reg ? n->reg[reg].sls : LT_SOCK_SLS;
        if (a.user[1] == PAP_SEND_STATUS) {
            uint8_t data[4 + 256] = {0};
            size_t sl = reg && n->ops.reg_status ? n->ops.reg_status(n->ops.ctx, reg, data + 4, 256) : 0;
            if (!sl) {
                status_bytes(n, data + 4);
                sl = 3;
            }
            uint8_t user[4] = { 0, PAP_STATUS, 0, 0 };
            respond(n, &from, sls, &a, user, data, 4 + sl, now);
            if (n->st.statuses++ == 0) say(n, "PAP SendStatus from %u", d->src.node);
        } else if (a.user[1] == PAP_OPEN_CONN) {
            pap_open(n, d, &a, &from, now, reg, sls);
        }
        return;
    }
    if (d->dst.sock != LT_SOCK_CONN || !p->open || a.user[0] != p->connid) return;
    if (d->src.node != p->ws.addr.node || d->src.net != p->ws.addr.net) return;
    p->heard = now;

    if (fn == ATP_TRESP && a.tid == p->rd_tid && p->rd_want && a.user[1] == PAP_DATA) {
        uint8_t seq = a.bitmap;
        p->wrote = p->idle_since = now;
        if (seq >= LT_FLOW || !(p->rd_want & (1u << seq))) return;
        size_t len = a.len < PAP_QUANTUM ? a.len : PAP_QUANTUM;
        memcpy(p->rd_buf[seq], a.data, len);
        p->rd_len[seq] = (uint16_t)len;
        p->rd_want &= (uint8_t)~(1u << seq);
        if (a.user[2]) p->rd_eof = 1;
        if (a.ctl & ATP_EOM) p->rd_want &= (uint8_t)((1u << (seq + 1)) - 1);
        if (!p->rd_want) pap_read_done(n, now);
        return;
    }
    if (fn != ATP_TREQ) return;
    switch (a.user[1]) {
    case PAP_TICKLE:
        break;                                     // heard; no response, by the spec
    case PAP_SEND_DATA: {
        uint16_t seq = (uint16_t)(a.user[2] << 8 | a.user[3]);
        n->st.ws_reads++;
        if (!p->ws_read && !p->ws_answered_seq)
            say(n, "PAP %u: the Mac reads us too (seq %u)", p->connid, seq);
        // A PAP-level retransmit of a read already answered (fresh TID, same
        // sequence number) gets the same answer again.
        if (seq && seq == p->ws_answered_seq) {
            uint8_t out[LT_RELAY_PKT];
            int end = p->ws_answered_eof;
            size_t k = relayed(n) ? n->ops.reg_output(n->ops.ctx, p->reg, out, sizeof out, &end, 1) : 0;
            pap_reply_ws(n, a.tid, &from, p->ws_answered_eof, out, k, now);
            break;
        }
        p->ws_read = 1;
        p->ws_read_tid = a.tid;
        p->ws_read_seq = seq;
        p->ws_read_from = from;
        pap_answer_ws_read(n, now);
        break;
    }
    case PAP_CLOSE_CONN: {
        uint8_t user[4] = { p->connid, PAP_CLOSE_CONN_REPLY, 0, 0 };
        respond(n, &from, LT_SOCK_CONN, &a, user, NULL, 0, now);
        pap_close(n, "by the Mac", 1);
        break;
    }
    default:
        break;
    }
}

static void pap_tick(lt_node_t *n, uint64_t now)
{
    lt_pap_t *p = &n->pap;
    if (!p->open) return;
    if (now - p->heard >= LT_PAP_CONN_TIMEOUT_MS) {
        pap_close(n, "timed out", 0);
        return;
    }
    if (n->idle_hold_ms && !relayed(n)) {          // a relay waits on its printer, not idle
        // Not idle: the printer's status holds the Mac, or we do (no read out
        // until the spool has room for one).
        if (n->status || (!p->rd_want && !p->done)) p->idle_since = now;
        else if (now - p->idle_since >= n->idle_hold_ms) {
            uint8_t user[4] = { p->connid, PAP_CLOSE_CONN, 0, 0 };
            send_atp(n, &p->ws, LT_SOCK_CONN, ATP_TREQ, 0x01, next_tid(n), user, NULL, 0);
            // What came prints; nothing at all is no print (W-1059's known-good mark).
            pap_close(n, "the Mac sent nothing", p->bytes > 0);
            return;
        }
    }
    if (now - p->tickled >= LT_PAP_TICKLE_MS) {
        uint8_t user[4] = { p->connid, PAP_TICKLE, 0, 0 };
        send_atp(n, &p->ws, LT_SOCK_CONN, ATP_TREQ, 0, p->tickle_tid, user, NULL, 0);
        p->tickled = now;
    }
    if (p->rd_want && now - p->rd_sent >= LT_PAP_SENDDATA_RETRY_MS) pap_send_read(n, now);
    pap_start_read(n, now);                        // when room has come back

    // Owed data: a read out, the printer ready, and the job begun.
    if (!(p->rd_want && n->status == 0 && p->bytes) || p->wrote > p->waiting) p->waiting = now;
    if (p->open && now - p->waiting >= LT_PAP_QUIET_MS) {
        uint8_t user[4] = { p->connid, PAP_CLOSE_CONN, 0, 0 };
        send_atp(n, &p->ws, LT_SOCK_CONN, ATP_TREQ, 0x01, next_tid(n), user, NULL, 0);
        pap_close(n, "the Mac went quiet", 1);
    }
    pap_answer_ws_read(n, now);
}

// ------------------------------------------------------------- W-1082

int lt_node_register(lt_node_t *n, int reg, const uint8_t *obj, const uint8_t *type, uint8_t sls)
{
    if (reg < 1 || reg >= LT_REG_MAX || !n->reg) return -1;
    if (!obj || !type || !sls) {
        memset(&n->reg[reg], 0, sizeof n->reg[reg]);
        return 0;
    }
    size_t ol = obj[0] > NBP_NAME_MAX ? NBP_NAME_MAX : obj[0];
    size_t tl = type[0] > NBP_NAME_MAX ? NBP_NAME_MAX : type[0];
    memcpy(n->reg[reg].obj + 1, obj + 1, ol);
    n->reg[reg].obj[0] = (uint8_t)ol;
    memcpy(n->reg[reg].type + 1, type + 1, tl);
    n->reg[reg].type[0] = (uint8_t)tl;
    n->reg[reg].sls = sls;
    return 0;
}

void lt_node_offer(lt_node_t *n, const uint8_t *own, const lt_offer_t *rel, int n_rel,
                   const uint8_t *marker)
{
    memset(n->obj, 0, sizeof n->obj);
    if (own && own[0]) memcpy(n->obj, own, 1 + (size_t)(own[0] > NBP_NAME_MAX ? NBP_NAME_MAX : own[0]));
    if (n->obj[0] > NBP_NAME_MAX) n->obj[0] = NBP_NAME_MAX;
    nbp_name(n->type, "ImageWriter");
    for (int r = 1; r < LT_REG_MAX - 1; r++) {
        int k = r - 1;
        if (k < n_rel && rel && rel[k].obj[0])
            lt_node_register(n, r, rel[k].obj, rel[k].type, (uint8_t)LT_SOCK_REG(r));
        else
            lt_node_register(n, r, NULL, NULL, 0);
    }
    uint8_t aw[NBP_NAME_MAX + 1];
    nbp_name(aw, LT_AIRWRITER_TYPE);
    if (marker && marker[0])
        lt_node_register(n, LT_REG_MAX - 1, marker, aw, (uint8_t)LT_SOCK_REG(LT_REG_MAX - 1));
    else
        lt_node_register(n, LT_REG_MAX - 1, NULL, NULL, 0);
}

int lt_node_pap_reg(const lt_node_t *n) { return n->pap.open ? n->pap.reg : 0; }

void lt_node_set_serve(lt_node_t *n, int serve)
{
    n->serve = serve ? 1 : 0;
}

void lt_node_lookup(lt_node_t *n, const char *type, uint64_t now)
{
    (void)now;
    if (n->phase != LT_ONLINE) return;
    nbp_tuple_t q = { .addr = { n->net, n->node, SOCK_NIS }, .enumerator = 0 };
    q.obj[0] = 1;
    q.obj[1] = NBP_WILD_ALL;
    nbp_name(q.type, type);
    q.zone[0] = 1;
    q.zone[1] = '*';
    uint8_t pkt[2 + 5 + 3 * (NBP_NAME_MAX + 1)];
    size_t pl = nbp_build(pkt, sizeof pkt, NBP_LKUP, ++n->lkup_id, &q);
    lt_route_t to = { { 0, LLAP_BCAST, SOCK_NIS }, LLAP_BCAST, n->link == LT_LINK_ELAP };
    if (pl) send_ddp(n, &to, SOCK_NIS, DDP_NBP, pkt, pl);
}

void lt_node_send_atp(lt_node_t *n, const lt_route_t *to, uint8_t src_sock, uint8_t ctl,
                      uint8_t bitmap, uint16_t tid, const uint8_t user[4],
                      const uint8_t *data, size_t len)
{
    send_atp(n, to, src_sock, ctl, bitmap, tid, user, data, len);
}

uint16_t lt_node_next_tid(lt_node_t *n) { return next_tid(n); }

void lt_node_attach(lt_node_t *n, struct pap_client *c)
{
    if (n->client && n->client != c) n->client->node = NULL;
    n->client = c;
    if (c) c->node = n;
}

// ------------------------------------------------------------- the rest

static void on_rtmp(lt_node_t *n, const ddp_t *d)
{
    if (d->src.sock != SOCK_RTMP || d->len < 4) return;
    if (n->link == LT_LINK_ELAP) {
        // Startup-range addresses only (W-1076): a router's ranges are not ours.
        if (!n->router_logged) say(n, "a router is on this network; not supported");
        n->router_logged = 1;
        return;
    }
    uint16_t net = (uint16_t)(d->data[0] << 8 | d->data[1]);
    if (net && net != n->net) {
        say(n, "network %u (router node %u)", net, d->data[3]);
        n->net = net;
    }
}

static void on_aep(lt_node_t *n, const ddp_t *d, uint64_t now)
{
    if (d->dst.sock != SOCK_AEP || d->len < 1) return;
    if (d->data[0] == 2) {
        n->st.echoes++;
        say(n, "echo reply from %u, %u bytes, %lu ms", d->src.node, (unsigned)d->len,
            (unsigned long)(now - n->echo_at));
        return;
    }
    if (d->data[0] != 1) return;
    uint8_t buf[DDP_MAX_DATA];
    memcpy(buf, d->data, d->len);
    buf[0] = 2;                                    // echo reply
    lt_route_t to = { d->src, d->llap_src, d->is_long };
    send_ddp(n, &to, SOCK_AEP, DDP_AEP, buf, d->len);
}

void lt_node_init(lt_node_t *n, const lt_cfg_t *cfg, const lt_ops_t *ops, uint64_t now)
{
    memset(n, 0, sizeof *n);
    n->ops = *ops;
    n->serve = 1;
    nbp_name(n->obj, cfg->object);
    nbp_name(n->type, cfg->type);
    lt_node_set_status(n, cfg->status);
    n->names = cfg->names;
    if (n->names) memset(n->names, 0, LT_SEEN_SLOTS * sizeof *n->names);
    n->reg = cfg->regs;
    if (n->reg) memset(n->reg, 0, LT_REG_MAX * sizeof *n->reg);
    n->rng = cfg->seed ? cfg->seed : 0x2434u;
    n->tid = (uint16_t)rnd(n);
    n->phase = LT_ACQUIRING;
    n->node = (cfg->hint >= 128 && cfg->hint <= 254) ? cfg->hint : new_candidate(n);
    n->candidates = 1;
    n->enq_at = now;
    n->ops.claim(n->ops.ctx, 0);
}

void lt_node_init_elap(lt_node_t *n, const lt_cfg_t *cfg, const lt_ops_t *ops,
                       uint32_t idle_hold_ms, uint64_t now)
{
    (void)now;
    memset(n, 0, sizeof *n);
    n->ops = *ops;
    n->serve = 1;
    nbp_name(n->obj, cfg->object);
    nbp_name(n->type, cfg->type);
    lt_node_set_status(n, cfg->status);
    n->names = cfg->names;
    if (n->names) memset(n->names, 0, LT_SEEN_SLOTS * sizeof *n->names);
    n->reg = cfg->regs;
    if (n->reg) memset(n->reg, 0, LT_REG_MAX * sizeof *n->reg);
    n->rng = cfg->seed ? cfg->seed : 0x2434u;
    n->tid = (uint16_t)rnd(n);
    n->phase = LT_ACQUIRING;                       // until AARP hands over an address
    n->link = LT_LINK_ELAP;
    n->idle_hold_ms = idle_hold_ms;
}

void lt_node_pap_abort(lt_node_t *n, uint64_t now)
{
    (void)now;
    lt_pap_t *p = &n->pap;
    if (!p->open) return;
    uint8_t user[4] = { p->connid, PAP_CLOSE_CONN, 0, 0 };
    send_atp(n, &p->ws, LT_SOCK_CONN, ATP_TREQ, 0x01, next_tid(n), user, NULL, 0);
    pap_close(n, "the printer gave up", 0);
}

void lt_node_echo(lt_node_t *n, uint8_t node, uint64_t now)
{
    if (n->phase != LT_ONLINE || !node || node == LLAP_BCAST) return;
    static const uint8_t req[] = { 1, 'A', 'i', 'r', 'W', 'r', 'i', 't', 'e', 'r' };
    lt_route_t to = { { n->net, node, SOCK_AEP }, node, n->link == LT_LINK_ELAP };
    send_ddp(n, &to, SOCK_AEP, DDP_AEP, req, sizeof req);
    n->echo_at = now;
    say(n, "echo request to %u", node);
}

void lt_node_set_status(lt_node_t *n, uint16_t status)
{
    n->status = status;
}

void lt_node_set_address(lt_node_t *n, uint16_t net, uint8_t node, uint64_t now)
{
    (void)now;
    if (n->link != LT_LINK_ELAP) return;
    if (n->pap.open && (net != n->net || node != n->node)) pap_close(n, "the address changed", 0);
    n->net = node ? net : 0;
    n->node = node;
    n->phase = node ? LT_ONLINE : LT_ACQUIRING;
    if (node) say(n, "address %u.%u is ours", net, node);
}

// A slot's node was heard within LT_SEEN_MS of now.
static int seen_live(const lt_node_t *n, int i, uint64_t now)
{
    return n->seen[i].node && (uint64_t)((uint32_t)(now / 1000) - n->seen[i].at_s) * 1000 < LT_SEEN_MS;
}

static int seen_find(const lt_node_t *n, uint16_t net, uint8_t node)
{
    if (n->link != LT_LINK_ELAP) net = 0;
    for (int i = 0; i < LT_SEEN_SLOTS; i++)
        if (n->seen[i].node == node && n->seen[i].net == net) return i;
    return -1;
}

static void seen_note(lt_node_t *n, uint16_t net, uint8_t node, uint64_t now)
{
    if (!node || node == LLAP_BCAST) return;
    if (n->link != LT_LINK_ELAP) net = 0;
    int k = seen_find(n, net, node);
    for (int i = 0; i < LT_SEEN_SLOTS && k < 0; i++)
        if (!n->seen[i].node) k = i;
    if (k < 0) {
        k = 0;
        for (int i = 1; i < LT_SEEN_SLOTS; i++)
            if (n->seen[i].at_s < n->seen[k].at_s) k = i;
    }
    if (n->seen[k].node != node || n->seen[k].net != net) n->seen[k].printer = 0;
    // New here, or back after a quiet spell: someone new until it says otherwise.
    if (n->seen[k].node != node || n->seen[k].net != net || !seen_live(n, k, now)) {
        n->seen[k].tries = 0;
        n->seen[k].ask_at = now + LT_NAME_DELAY_MS;
        if (n->names) n->names[k][0] = 0;
    }
    n->seen[k].net = net;
    n->seen[k].node = node;
    n->seen[k].at_s = (uint32_t)(now / 1000);
}

static const uint8_t *seen_name(const lt_node_t *n, int i)
{
    return i >= 0 && n->names && n->names[i][0] ? n->names[i] : NULL;
}

static int seen_printer(const lt_node_t *n, int i)
{
    return i >= 0 && n->seen[i].printer && !seen_name(n, i);
}

// Its own answer marks a printer: one a router relays comes from the router.
static void printer_note(lt_node_t *n, const ddp_t *d, const lt_addr_t *a)
{
    if (a->node != d->llap_src || d->src.node != d->llap_src) return;
    n->printers[a->node / 8] |= (uint8_t)(1u << (a->node % 8));
    int i = seen_find(n, d->src.net, a->node);
    if (i >= 0) n->seen[i].printer = 1;
}

size_t lt_node_seen(const lt_node_t *n, uint64_t now, uint32_t within_ms, lt_seen_t *out, size_t max)
{
    uint32_t t = (uint32_t)(now / 1000);
    lt_seen_t all[LT_SEEN_SLOTS];
    size_t k = 0;
    for (int i = 0; i < LT_SEEN_SLOTS; i++) {
        if (!n->seen[i].node || (uint64_t)(t - n->seen[i].at_s) * 1000 >= within_ms ||
            seen_printer(n, i))
            continue;
        lt_seen_t s = { n->seen[i].node, t - n->seen[i].at_s, seen_name(n, i) };
        size_t j = k++;
        while (j > 0 && all[j - 1].ago_s > s.ago_s) { all[j] = all[j - 1]; j--; }   // newest first
        all[j] = s;
    }
    if (k > max) k = max;
    memcpy(out, all, k * sizeof *out);
    return k;
}

const uint8_t *lt_node_name(const lt_node_t *n, uint16_t net, uint8_t node)
{
    return seen_name(n, seen_find(n, net, node));
}

int lt_node_is_printer(const lt_node_t *n, uint16_t net, uint8_t node)
{
    return seen_printer(n, seen_find(n, net, node));
}

// ------------------------------------------------------------- names

// Ask net.node for =:Workstation@*, its answer to our NIS.
static void name_ask(lt_node_t *n, uint16_t net, uint8_t node)
{
    nbp_tuple_t q = { .addr = { n->net, n->node, SOCK_NIS }, .enumerator = 0 };
    q.obj[0] = 1;
    q.obj[1] = '=';
    nbp_name(q.type, LT_NAME_TYPE);
    q.zone[0] = 1;
    q.zone[1] = '*';
    uint8_t pkt[2 + 5 + 3 * (NBP_NAME_MAX + 1)];
    size_t pl = nbp_build(pkt, sizeof pkt, NBP_LKUP, ++n->name_id, &q);
    lt_route_t to = { { net, node, SOCK_NIS }, node, n->link == LT_LINK_ELAP };
    if (pl) send_ddp(n, &to, SOCK_NIS, DDP_NBP, pkt, pl);
}

static void names_tick(lt_node_t *n, uint64_t now)
{
    if (!n->names) return;
    int due = -1;
    for (int i = 0; i < LT_SEEN_SLOTS; i++) {
        if (!seen_live(n, i, now) || n->seen[i].ask_at > now) continue;
        if (n->seen[i].tries >= LT_NAME_TRIES)    // the last ask went unanswered too
            n->names[i][0] = 0;
        if (due < 0 || n->seen[i].ask_at < n->seen[due].ask_at) due = i;
    }
    if (due < 0 || now - n->name_at < LT_NAME_GAP_MS) return;
    name_ask(n, n->seen[due].net, n->seen[due].node);
    if (n->seen[due].tries < 0x7F) n->seen[due].tries++;
    // Each wait for an answer twice the last, up to the refresh: a Mac
    // starting up may be minutes from registering its name.
    unsigned k = n->seen[due].tries - 1u;
    uint64_t wait = (uint64_t)LT_NAME_RETRY_MS << (k < 8 ? k : 8);
    n->seen[due].ask_at = now + (wait < LT_NAME_REFRESH_MS ? wait : LT_NAME_REFRESH_MS);
    n->name_at = now;
}

// A Mac registering a name looks it up first, a few times a second apart, to
// see that no one has it: <its name>:Workstation@* from it is its own. Its
// next ask is a moment after the last of those, when the name is in.
static void name_registering(lt_node_t *n, const ddp_t *d, const nbp_t *q, uint64_t now)
{
    uint8_t ws[NBP_NAME_MAX + 1];
    nbp_name(ws, LT_NAME_TYPE);
    if (!n->names || !q->t.obj[0] || (q->t.obj[0] == 1 && q->t.obj[1] == NBP_WILD_ALL) ||
        !nbp_match(ws, q->t.type))
        return;
    int i = seen_find(n, d->src.net, d->src.node);
    if (i < 0) return;
    n->seen[i].tries = 0;
    n->seen[i].ask_at = now + LT_NAME_DELAY_MS;
}

static void name_reply(lt_node_t *n, const ddp_t *d, const nbp_t *q, uint64_t now)
{
    uint8_t ws[NBP_NAME_MAX + 1];
    nbp_name(ws, LT_NAME_TYPE);
    if (!n->names || !q->count || !q->t.obj[0] || !nbp_match(ws, q->t.type)) return;
    uint16_t net = q->t.addr.net ? q->t.addr.net : d->src.net;
    uint8_t node = q->t.addr.node ? q->t.addr.node : d->src.node;
    int i = seen_find(n, net, node);
    if (i < 0) return;
    n->seen[i].tries = 0;
    n->seen[i].ask_at = now + LT_NAME_REFRESH_MS;
    if (!memcmp(n->names[i], q->t.obj, 1 + (size_t)q->t.obj[0])) return;
    memcpy(n->names[i], q->t.obj, 1 + (size_t)q->t.obj[0]);
    char u[3 * NBP_NAME_MAX + 1];
    nbp_utf8(u, sizeof u, q->t.obj);
    if (n->link == LT_LINK_ELAP) say(n, "%u.%u is %s", net, node, u);
    else say(n, "node %u is %s", node, u);
}

void lt_node_rx(lt_node_t *n, const uint8_t *f, size_t len, uint64_t now)
{
    if (len < LLAP_HDR) return;
    n->st.frames++;
    uint8_t dst = f[0], src = f[1], type = f[2];
    // ENQs carry the prober's candidate as their source: not a node yet.
    if (type != LLAP_ENQ) {
        n->heard[src / 8] |= (uint8_t)(1u << (src % 8));
        uint16_t net = 0;                          // ELAP: the long header's source network
        if (n->link == LT_LINK_ELAP && type == LLAP_LONG && len >= LLAP_HDR + DDP_LONG_HDR)
            net = (uint16_t)(f[LLAP_HDR + 6] << 8 | f[LLAP_HDR + 7]);
        seen_note(n, net, src, now);
    }

    if (n->phase == LT_ACQUIRING) {
        if (n->link == LT_LINK_ELAP) return;
        // Appendix B: while the ID is tentative, anything addressed to it --
        // an ACK to our ENQ, another node's ENQ for the same ID, data -- or
        // anything sent from it means it is taken.
        if ((dst == n->node && dst != LLAP_BCAST) || (src == n->node && type != LLAP_ENQ))
            n->clash = 1;
        return;
    }
    if (dst != n->node && dst != LLAP_BCAST) return;
    // Someone else sending as us. TashTalk does not hear its own frames (U8 is
    // off while the PIC transmits), so this is a duplicate ID; count it.
    if (n->link == LT_LINK_LLAP && src == n->node && type != LLAP_ENQ && type != LLAP_CTS)
        n->st.dup_suspect++;
    if (type != LLAP_SHORT && type != LLAP_LONG) return;

    ddp_t d;
    if (ddp_parse(f, len, &d)) return;
    if (d.dst.node != n->node && d.dst.node != LLAP_BCAST) return;
    if (n->link == LT_LINK_ELAP && d.dst.net && d.dst.net != n->net) return;
    n->st.for_us++;
    switch (d.type) {
    case DDP_NBP:  on_nbp(n, &d, now); break;
    case DDP_ATP:  on_atp(n, &d, now); break;
    case DDP_RTMP: on_rtmp(n, &d); break;
    case DDP_AEP:  on_aep(n, &d, now); break;
    default: break;
    }
}

void lt_node_tick(lt_node_t *n, uint64_t now)
{
    if (n->phase == LT_ONLINE) {
        pap_tick(n, now);
        names_tick(n, now);
        if (n->client) papc_tick(n->client, now);
        return;
    }
    if (n->link == LT_LINK_ELAP) return;
    if (n->clash) {
        uint8_t was = n->node;
        n->node = new_candidate(n);
        n->candidates++;
        n->clash = 0;
        n->enq_sent = 0;
        n->enq_at = now;
        say(n, "node %u is taken; trying %u", was, n->node);
    }
    if (now < n->enq_at) return;
    if (n->enq_sent >= LT_ENQ_TRIES) {
        n->phase = LT_ONLINE;
        n->ops.claim(n->ops.ctx, n->node);
        say(n, "node %u is ours (%lu enquiries, %lu candidate%s)", n->node,
            (unsigned long)n->enq_sent, (unsigned long)n->candidates, n->candidates == 1 ? "" : "s");
        return;
    }
    uint8_t enq[LLAP_HDR] = { n->node, n->node, LLAP_ENQ };
    n->ops.send(n->ops.ctx, enq, sizeof enq);
    n->enq_sent++;
    n->enq_at = now + LT_ENQ_GAP_MS;
}
