// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a QuickDraw LaserWriter's conversation (qd_session.h).
#include "qd_session.h"
#include "qd_wire.h"
#include "bigmem.h"
#include <string.h>

#define REPLY_MS   1000        // a reply's wait; the 300 answers an 83 in about 30 ms
#define POLL_MS     500        // how often a page's progress or the paper is asked
#define BLINK_MS   1000        // the Ready light's half period while a page loads and prints
#define DONE_MS   85000        // Apple's limit for a page to come out after 8A 01
#define STALL_MS 600000
#define RESET_MS    280        // 84 08 held during a jam's recovery, as Apple's driver holds it

struct qd_session {
    qd_ops_t         o;
    qd_job_cfg_t     c;
    const qd_spec_t *sp;
    const qd_media_t *md;
    uint32_t stall;
    uint8_t  lights;            // what the last 84 carried
    uint32_t blink_at;
    uint32_t rows;
    int      wrote, closed;
    int      have_st;
    qd_state_t st;
    uint8_t  pkt[QD_ROW_MAX + 3];
};

// ---- the line, with or without a session

static int put_cmd(const qd_ops_t *o, uint8_t c, uint8_t a)
{
    uint8_t p[3];
    qd_cmd(c, a, p);
    return o->put(o->ctx, p, 3);
}

// One question and its byte: >= 0 the reply, else a QD_E_ (QD_E_OFFLINE: no reply after the
// tries). An 83 is tried five times while its reply is refused, garbled or missing, 82 and 8E
// twice. quick: a try that gets nothing ends the question at once (the done wait's polls).
static int ask(const qd_ops_t *o, uint8_t c, uint8_t a, int quick)
{
    int tries = c == 0x83 ? 5 : 2;
    for (int i = 0; i < tries; i++) {
        if (o->drain) o->drain(o->ctx);
        int rc = put_cmd(o, c, a);
        if (!rc) rc = o->flush(o->ctx);
        if (rc) return rc;
        uint8_t b;
        if (o->read(o->ctx, &b, 1, REPLY_MS, 0) != 1) {
            if (quick) return QD_E_OFFLINE;
            continue;
        }
        // 8E's 00 says the status is the next byte.
        if (c == 0x8E && b == 0x00 && o->read(o->ctx, &b, 1, REPLY_MS, 0) != 1) {
            if (quick) return QD_E_OFFLINE;
            continue;
        }
        if (c == 0x83 && !qd_reply_ok(b)) continue;
        return b;
    }
    return QD_E_OFFLINE;
}

static int offline(const qd_ops_t *o, int rc)
{
    if (rc == QD_E_OFFLINE && o->state) o->state(o->ctx, QDS_OFFLINE);
    return rc;
}

// ---- idle

int qd_ask_identity(const qd_ops_t *o, uint8_t *engine, uint8_t *ctrl)
{
    int rc = put_cmd(o, 0x81, 0x00);
    if (rc) return offline(o, rc);
    int v;
    if ((v = ask(o, 0x83, 0x40, 0)) < 0) return offline(o, v);
    if ((v = ask(o, 0x83, 0x3E, 0)) < 0) return offline(o, v);
    *engine = (uint8_t)v;
    if ((v = ask(o, 0x82, 0x00, 0)) < 0) return offline(o, v);
    *ctrl = (uint8_t)v;
    return QD_OK;
}

// A status question that got nothing: which, for the log (qd_raw_t.miss).
static int status_offline(const qd_ops_t *o, qd_raw_t *raw, uint8_t c, uint8_t a, int rc)
{
    if (raw && rc == QD_E_OFFLINE) raw->miss = (uint16_t)(c << 8 | a);
    return offline(o, rc);
}

int qd_ask_status(const qd_ops_t *o, qd_model_t m, int manual, qd_state_t *st, qd_raw_t *raw)
{
    const qd_spec_t *sp = qd_spec(m);
    if (!sp) return QD_E_MODEL;
    if (raw) raw->miss = 0;
    static const uint8_t which[4] = { 0x01, 0x02, 0x2A, 0x00 };
    int v[4];
    for (int i = 0; i < 4; i++) {
        uint8_t c = i == 3 ? 0x82 : 0x83;
        if ((v[i] = ask(o, c, which[i], 0)) < 0) return status_offline(o, raw, c, which[i], v[i]);
    }
    qd_raw_t r = { .s01 = (uint8_t)v[0], .s02 = (uint8_t)v[1], .s2a = (uint8_t)v[2], .c82 = (uint8_t)v[3] };
    if (r.s01 & 0x12) {
        int a = ask(o, 0x83, 0x04, 0), b = a < 0 ? a : ask(o, 0x83, 0x07, 0);
        if (a < 0) return status_offline(o, raw, 0x83, 0x04, a);
        if (b < 0) return status_offline(o, raw, 0x83, 0x07, b);
        r.s04 = (uint8_t)a;
        r.s07 = (uint8_t)b;
    }
    *st = qd_state_from(sp, &r, manual);
    if (raw) *raw = r;
    int rc = put_cmd(o, 0x84, qd_lights(*st, 1));
    if (!rc) rc = o->flush(o->ctx);
    return rc ? offline(o, rc) : QD_OK;
}

// ---- a job

static uint32_t now(const qd_session_t *s) { return s->o.now_ms(s->o.ctx); }
static uint32_t since(const qd_session_t *s, uint32_t t) { return now(s) - t; }

static void state(qd_session_t *s, qd_state_t st)
{
    if (s->have_st && s->st == st) return;
    s->have_st = 1;
    s->st = st;
    if (s->o.state) s->o.state(s->o.ctx, st);
}

static int fail(qd_session_t *s, int rc)
{
    if (rc == QD_E_OFFLINE) state(s, QDS_OFFLINE);
    return rc;
}

static int cmd(qd_session_t *s, uint8_t c, uint8_t a)
{
    s->wrote = 1;
    int rc = put_cmd(&s->o, c, a);
    return rc ? fail(s, rc) : 0;
}

static int q(qd_session_t *s, uint8_t c, uint8_t a)
{
    s->wrote = 1;
    int v = ask(&s->o, c, a, 0);
    return v < 0 ? fail(s, v) : v;
}

static int flush(qd_session_t *s)
{
    int rc = s->o.flush(s->o.ctx);
    return rc ? fail(s, rc) : 0;
}

static int lights(qd_session_t *s, uint8_t v)
{
    s->lights = v;
    return cmd(s, 0x84, v);
}

static int cancelled(qd_session_t *s)
{
    return s->o.cancelled && s->o.cancelled(s->o.ctx);
}

static int cancel(qd_session_t *s)
{
    cmd(s, 0x8A, 0x00);
    cmd(s, 0x81, 0x00);
    cmd(s, 0x84, 0x01);
    s->o.flush(s->o.ctx);
    s->closed = 1;
    return QD_E_CANCEL;
}

// The Ready light blinks about once a second while a page loads and prints.
static int blink(qd_session_t *s)
{
    if (since(s, s->blink_at) < BLINK_MS) return 0;
    s->blink_at = now(s);
    return lights(s, (uint8_t)(s->lights ^ 0x01));
}

static uint8_t source(const qd_session_t *s) { return s->c.manual ? s->sp->src_manual : s->sp->src_cassette; }
static uint8_t paper_bit(const qd_session_t *s) { return s->c.manual ? s->sp->bit_manual : s->sp->bit_cassette; }

qd_session_t *qd_session_new(const qd_ops_t *o, const qd_job_cfg_t *c)
{
    const qd_spec_t *sp = qd_spec(c->model);
    if (!sp) return NULL;
    qd_session_t *s = big_calloc(1, sizeof *s);
    if (!s) return NULL;
    s->o = *o;
    s->c = *c;
    s->sp = sp;
    s->stall = c->stall_ms ? c->stall_ms : STALL_MS;
    s->lights = 0x01;
    return s;
}

void qd_session_free(qd_session_t *s)
{
    free(s);
}

static int identity(qd_session_t *s)
{
    int v;
    if ((v = cmd(s, 0x81, 0x00))) return v;
    if ((v = q(s, 0x83, 0x40)) < 0) return v;
    int e = q(s, 0x83, 0x3E);
    if (e < 0) return e;
    if ((v = q(s, 0x82, 0x00)) < 0) return v;
    if (e != s->sp->engine) {
        s->closed = 1;               // another printer: nothing more goes to it
        return QD_E_MODEL;
    }
    return 0;
}

int qd_session_begin(qd_session_t *s)
{
    int rc = identity(s);
    if (rc) return rc;
    state(s, QDS_PRINTING);
    // A page still coming out of the printer finishes first.
    uint32_t t0 = now(s);
    for (;;) {
        int v = q(s, 0x83, 0x01);
        if (v < 0) return v;
        if (!(v & 0x20)) return QD_OK;
        if (cancelled(s)) return cancel(s);
        if (since(s, t0) >= s->stall) return QD_E_STALL;
        s->o.sleep_ms(s->o.ctx, POLL_MS);
    }
}

int qd_session_open(qd_session_t *s, const qd_media_t *md)
{
    s->md = md;
    qd_setup_t u = qd_setup(md);
    int v = identity(s);
    if (v) return v;
    if ((v = cmd(s, 0x87, (uint8_t)u.left))) return v;
    if ((v = cmd(s, 0x85, (uint8_t)u.top))) return v;
    if ((v = cmd(s, 0x86, (uint8_t)((uint16_t)u.top >> 8)))) return v;
    if ((v = q(s, 0x83, 0x9D)) < 0) return v;
    if ((v = q(s, 0x83, qd_value(0x21))) < 0) return v;
    if ((v = q(s, 0x83, s->sp->density_cmd)) < 0) return v;
    if ((v = q(s, 0x83, qd_value(s->sp->density))) < 0) return v;
    if ((v = cmd(s, 0x84, 0x00))) return v;
    if ((v = cmd(s, 0x88, (uint8_t)u.width))) return v;
    if ((v = cmd(s, 0x89, (uint8_t)((uint16_t)u.width >> 8)))) return v;
    if ((v = q(s, 0x83, source(s))) < 0) return v;
    if ((v = lights(s, 0x01))) return v;
    if ((v = flush(s))) return v;
    s->blink_at = now(s);
    s->rows = 0;
    return QD_OK;
}

int qd_session_row(qd_session_t *s, const uint8_t *codes, size_t n)
{
    if (!(s->rows & 31) && cancelled(s)) return cancel(s);
    size_t len = qd_row_packet(codes, n, s->pkt);
    if (!len) return QD_E_FAULT;
    s->wrote = 1;
    int rc = s->o.put(s->o.ctx, s->pkt, len);
    if (rc) return fail(s, rc);
    s->rows++;
    return blink(s);
}

// Paper at the source the job asked for: 83 2A every half second, the state and the Paper
// light while it is not there, up to the stall limit.
static int wait_paper(qd_session_t *s)
{
    uint32_t t0 = now(s);
    for (;;) {
        int a = q(s, 0x83, 0x2A);
        if (a < 0) return a;
        if (a & paper_bit(s)) return 0;
        qd_state_t st = QDS_MANUAL;
        if (!s->c.manual) {
            int b = q(s, 0x83, 0x02);
            if (b < 0) return b;
            st = (b & 0x20) ? QDS_NO_CASSETTE : QDS_NO_PAPER;
        }
        state(s, st);
        int rc = lights(s, qd_lights(st, 0));
        if (!rc) rc = flush(s);
        if (rc) return rc;
        if (cancelled(s)) return cancel(s);
        if (since(s, t0) >= s->stall) return QD_E_STALL;
        s->o.sleep_ms(s->o.ctx, POLL_MS);
    }
}

// A jam or the cover: stop, show it, wait for it to clear, then Apple's recovery, which prints
// the page again from the printer's memory.
static int recover(qd_session_t *s, uint8_t s02)
{
    qd_state_t st = (s02 & 0x08) ? QDS_JAM : QDS_COVER;
    int rc = cmd(s, 0x8A, 0x00);
    if (rc) return rc;
    state(s, st);
    if ((rc = lights(s, qd_lights(st, 0))) || (rc = flush(s))) return rc;
    uint32_t t0 = now(s);
    for (;;) {
        s->o.sleep_ms(s->o.ctx, POLL_MS);
        if (cancelled(s)) return cancel(s);
        int b = q(s, 0x83, 0x02);
        if (b < 0) return b;
        if (!(b & 0x0C)) break;
        if (since(s, t0) >= s->stall) return QD_E_STALL;
    }
    if ((rc = cmd(s, 0x81, 0x00)) || (rc = cmd(s, 0x84, 0x08)) || (rc = flush(s))) return rc;
    s->o.sleep_ms(s->o.ctx, RESET_MS);
    if ((rc = lights(s, 0x01))) return rc;
    static const uint8_t again[4] = { 0x40, 0x3E, 0x00, 0x5D };
    for (int i = 0; i < 4; i++)
        if ((rc = q(s, i == 2 ? 0x82 : 0x83, again[i])) < 0) return rc;
    if ((rc = q(s, 0x83, source(s))) < 0) return rc;
    if ((rc = cmd(s, 0x81, 0x00)) || (rc = cmd(s, 0x8A, 0x01)) || (rc = flush(s))) return rc;
    state(s, QDS_PRINTING);
    return 0;
}

// The printer stops answering: two polls in a row with no reply.
static int missed(qd_session_t *s, int v, int *misses)
{
    if (v != QD_E_OFFLINE) return fail(s, v);
    return ++*misses >= 2 ? fail(s, QD_E_OFFLINE) : 0;
}

static int wait_done(qd_session_t *s)
{
    const qd_ops_t *o = &s->o;
    uint32_t printed = now(s), poll = printed;
    int misses = 0;
    for (;;) {
        // The next half second, or at once when the printer sends a BREAK.
        while (since(s, poll) < POLL_MS) {
            if (o->brk && o->brk(o->ctx)) break;
            if (cancelled(s)) return cancel(s);
            uint32_t left = POLL_MS - since(s, poll);
            o->sleep_ms(o->ctx, left < 50 ? left : 50);
        }
        poll = now(s);
        if (cancelled(s)) return cancel(s);
        if (since(s, printed) >= DONE_MS) return QD_E_FAULT;

        int e = ask(o, 0x8E, 0x00, 1);
        if (e < 0) {
            int rc = missed(s, e, &misses);
            if (rc) return rc;
            continue;
        }
        if (e & 0x04) return QD_OK;
        int s01 = ask(o, 0x83, 0x01, 1);
        int s02 = s01 < 0 ? s01 : ask(o, 0x83, 0x02, 1);
        if (s02 < 0) {
            int rc = missed(s, s02, &misses);
            if (rc) return rc;
            continue;
        }
        misses = 0;
        int rc;
        if (s02 & 0x0C) {
            if ((rc = recover(s, (uint8_t)s02))) return rc;
            printed = poll = now(s);
            continue;
        }
        if (s01 & 0x02) {
            if ((rc = q(s, 0x83, 0x04)) < 0 || (rc = q(s, 0x83, 0x07)) < 0) return rc;
            state(s, QDS_ENGINE);
            return QD_E_FAULT;
        }
        if (!(s01 & 0x20)) {
            // Not moving: is the paper gone? Then the page waits in the printer for more.
            int a = ask(o, 0x83, 0x2A, 1);
            if (a < 0) {
                if ((rc = missed(s, a, &misses))) return rc;
                continue;
            }
            if (!(a & paper_bit(s))) {
                if ((rc = cmd(s, 0x8A, 0x00)) || (rc = wait_paper(s))) return rc;
                if ((rc = lights(s, 0x01)) || (rc = cmd(s, 0x8A, 0x01)) || (rc = flush(s))) return rc;
                state(s, QDS_PRINTING);
                printed = poll = now(s);
                continue;
            }
        }
        if ((rc = blink(s)) || (rc = flush(s))) return rc;
    }
}

int qd_session_print(qd_session_t *s)
{
    int rc = flush(s);
    if (rc) return rc;
    // Did the page arrive whole? Memory full in either, or 8E's error bit: it never prints.
    int e = q(s, 0x8E, 0x00);
    if (e < 0) return e;
    int c = q(s, 0x82, 0x00);
    if (c < 0) return c;
    if ((e & 0x03) || (c & 0x01)) return QD_E_FAULT;
    if ((rc = wait_paper(s))) return rc;
    if ((rc = lights(s, 0x01)) || (rc = cmd(s, 0x8B, 0x8B)) || (rc = cmd(s, 0x8B, 0x8B)) ||
        (rc = cmd(s, 0x8A, 0x01)))
        return rc;
    state(s, QDS_PRINTING);
    if ((rc = flush(s))) return rc;
    s->blink_at = now(s);
    return wait_done(s);
}

int qd_session_end(qd_session_t *s)
{
    // Page done comes while the last sheet is still on its way out (83 01 bit 5, about 15 s
    // more on board 1): the job ends Printing then, and the status at rest says Ready once the
    // sheet is out, rather than Ready now and Printing again at the next status. One quick
    // try: a reply missed is Ready.
    int v = ask(&s->o, 0x83, 0x01, 1);
    int rc = v == QD_E_OFFLINE || v >= 0 ? 0 : v;
    if (!rc) rc = cmd(s, 0x81, 0x00);
    if (!rc) rc = lights(s, 0x01);
    if (!rc) rc = flush(s);
    s->closed = 1;
    if (!rc) state(s, v >= 0 && (v & 0x20) ? QDS_PRINTING : QDS_READY);
    return rc;
}

void qd_session_abort(qd_session_t *s)
{
    if (!s->wrote || s->closed) return;
    s->closed = 1;
    put_cmd(&s->o, 0x8A, 0x00);
    put_cmd(&s->o, 0x81, 0x00);
    put_cmd(&s->o, 0x84, 0x01);
    s->o.flush(s->o.ctx);
}
