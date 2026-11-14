// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a QuickDraw LaserWriter's conversation (qd_session.h). W-1144: the next page loads
// while one prints.
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
#define CLEAR_MS   3000        // how long 83 02 must stay clear before a jam counts as cleared
#define OUT_MS    20000        // a sheet is on its way out this long after its page done
#define AGAIN     (-100)       // inside a resend: a jam again; never returned to a caller

// A page in the copy of what the printer holds: its rows, [u16 LE n][codes] each, in the ring.
enum { CP_LOADING, CP_PRINTING, CP_DONE };
enum { CP_MAX = 4 };
typedef struct {
    int      no;
    const qd_media_t *md;
    uint32_t start, bytes, rows;
    uint8_t  state, complete;
    uint32_t done_at;
} cpage_t;

struct qd_session {
    qd_ops_t         o;
    qd_job_cfg_t     c;
    const qd_spec_t *sp;
    uint32_t stall, room;
    uint8_t  lights;            // what the last 84 carried
    uint32_t blink_at;
    int      wrote, closed;
    int      err;               // the first error a call returned: the printer's, or a cancel
    int      stopping;          // the job is ending early: a fault stops it at once
    int      soft;              // the error was the line's, not the printer's (abort)
    int      fault;             // a jam or the cover shows: nothing but 8A 00 goes
    int      have_st;
    qd_state_t st;
    // The page printing: its 8A 01 has gone and its done has not been read.
    int      printing;
    int      p_no;
    const qd_media_t *p_md;
    uint32_t p_bytes;           // its codes, in the printer until its done
    uint32_t p_at;              // when its 8A 01 last went
    int      p_first;           // the first printed since the last 81 00: at the start of the memory
    // The page loading: opened and not yet printed.
    int      loading;
    int      l_no;
    const qd_media_t *l_md;
    uint32_t l_bytes;
    uint32_t rows;
    uint8_t  e_bits;            // 8E's bits 0 and 1, from every read since its open
    int      printed;           // pages printed since the last 81 00
    int      rewound;           // a recovery's 81 00 went: the next page starts the memory over
    unsigned recoveries;
    // The status asked between rows and while a page prints.
    uint32_t poll_at;           // the last one between rows
    int      misses;            // polls in a row with no answer
    int      late;              // the last 8E went unanswered: its reply may still come
    // The copy (qd_job_cfg_t.copy): the pages the printer holds or has just let go, oldest
    // first, end to end in the ring.
    cpage_t  cp[CP_MAX];
    int      ncp;
    uint32_t cused;             // bytes of the ring the pages take, from cp[0].start
    int      cl, cc;            // the page loading's and the page printing's, or -1
    int      rip_open;          // the last is the rip's own page, opened and not yet printed
    int      resending;         // its pages are going to the printer again after a jam
    uint8_t  again_s02;
    uint8_t  row_tmp[QD_ROW_MAX];
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

// A job that took or asked for the manual slot sets the cassette again as it stops, however it
// stops: the printer otherwise keeps the slot and pulls in the next sheet put there (board 1,
// 2026-11-13). Its reply waits for the next question's drain.
static void cassette_again(qd_session_t *s)
{
    if (s->c.manual) put_cmd(&s->o, 0x83, 0x4C);
}

static void event(qd_session_t *s, qd_event_t ev, int page, uint32_t ms)
{
    if (s->o.event) s->o.event(s->o.ctx, ev, page, ms);
}

// The job stops here: whatever prints is stopped, and what is loaded is dropped. While a jam
// or the cover shows, only the stop goes: 81 00 sent into a printer clearing a jam jammed it
// again (bench A4e); the next job's 81 00 drops what is loaded.
static int stop_now(qd_session_t *s)
{
    cmd(s, 0x8A, 0x00);
    cassette_again(s);
    if (!s->fault) {
        cmd(s, 0x81, 0x00);
        cmd(s, 0x84, 0x01);
    }
    s->o.flush(s->o.ctx);
    s->closed = 1;
    s->printing = 0;
    return QD_E_CANCEL;
}

static int stopping(qd_session_t *s)
{
    return s->stopping || cancelled(s);
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
    s->room = c->room ? c->room : sp->budget;
    s->lights = 0x01;
    s->cl = s->cc = -1;
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

static int begin(qd_session_t *s)
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
        if (cancelled(s)) return stop_now(s);
        if (since(s, t0) >= s->stall) return QD_E_STALL;
        s->o.sleep_ms(s->o.ctx, POLL_MS);
    }
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
            st = (b & QD_S02_CASSETTE) ? QDS_NO_CASSETTE : QDS_NO_PAPER;
        }
        state(s, st);
        int rc = lights(s, qd_lights(st, 0));
        if (!rc) rc = flush(s);
        if (rc) return rc;
        if (stopping(s)) return stop_now(s);
        if (since(s, t0) >= s->stall) return QD_E_STALL;
        s->o.sleep_ms(s->o.ctx, POLL_MS);
    }
}

// ---- the copy of what is in the printer (W-1144)

static int copying(const qd_session_t *s) { return s->c.copy && s->c.copy_cap; }

// Lets go of the oldest page, which the caller knows is done.
static void copy_drop(qd_session_t *s)
{
    s->cused -= s->cp[0].bytes;
    memmove(s->cp, s->cp + 1, (size_t)(--s->ncp) * sizeof s->cp[0]);
    if (s->cl >= 0) s->cl--;
    if (s->cc >= 0) s->cc--;
}

// Only the newest page that is done is worth keeping: its sheet may still be on its way out.
static void copy_trim(qd_session_t *s)
{
    if (s->resending) return;
    while (s->ncp > 1 && s->cp[0].state == CP_DONE && s->cp[1].state == CP_DONE) copy_drop(s);
}

// A page's record begins: the rip's new page, or its open page from the top again.
static void copy_open(qd_session_t *s, int page, const qd_media_t *md, int again)
{
    if (!copying(s) || s->resending) return;
    cpage_t *c;
    if (again && s->rip_open && s->ncp) {
        c = &s->cp[s->ncp - 1];
        s->cused -= c->bytes;
    } else {
        copy_trim(s);
        while (s->ncp >= CP_MAX && s->cp[0].state == CP_DONE) copy_drop(s);
        if (s->ncp >= CP_MAX) {             // never: a page done, one printing, one loading
            s->cl = -1;
            return;
        }
        c = &s->cp[s->ncp++];
        const cpage_t *b = s->ncp > 1 ? c - 1 : NULL;
        c->start = b ? (uint32_t)((b->start + b->bytes) % s->c.copy_cap) : 0;
    }
    c->no = page;
    c->md = md;
    c->bytes = c->rows = 0;
    c->state = CP_LOADING;
    c->complete = 1;
    s->cl = (int)(c - s->cp);
    s->rip_open = 1;
}

static void ring_put(qd_session_t *s, uint32_t at, const uint8_t *b, size_t n)
{
    const size_t cap = s->c.copy_cap;
    at %= cap;
    size_t first = n < cap - at ? n : cap - at;
    memcpy(s->c.copy + at, b, first);
    memcpy(s->c.copy, b + first, n - first);
}

static void ring_get(const qd_session_t *s, uint32_t at, uint8_t *b, size_t n)
{
    const size_t cap = s->c.copy_cap;
    at %= cap;
    size_t first = n < cap - at ? n : cap - at;
    memcpy(b, s->c.copy + at, first);
    memcpy(b + first, s->c.copy, n - first);
}

// A row that went to the printer goes into the loading page's copy. Room comes from the
// oldest page that is done, never from one still in the printer: with none to be had, this
// page's copy is given up, and a jam finds it has none.
static void copy_row(qd_session_t *s, const uint8_t *codes, size_t n)
{
    if (!copying(s) || s->resending || s->cl < 0) return;
    cpage_t *c = &s->cp[s->cl];
    if (!c->complete) return;
    while (s->cused + 2 + n > s->c.copy_cap) {
        if (s->cl == 0 || s->cp[0].state != CP_DONE) {
            c->complete = 0;
            return;
        }
        copy_drop(s);
        c = &s->cp[s->cl];
    }
    const uint8_t head[2] = { (uint8_t)n, (uint8_t)(n >> 8) };
    ring_put(s, c->start + c->bytes, head, 2);
    ring_put(s, c->start + c->bytes + 2, codes, n);
    c->bytes += (uint32_t)(2 + n);
    c->rows++;
    s->cused += (uint32_t)(2 + n);
}

// A jam or the cover shows (83 02 bits 3 and 2): the state and the Jam light, and nothing
// sent but the question until it is gone. Gone is 83 02 clear on reads 3 s apart, and nothing
// moving (83 01 bit 5): board 1's printer read clear once and jammed again with a second
// sheet still in its path, and feeds by itself once a jam clears (bench A4d, A4e).
static int wait_clear(qd_session_t *s, uint8_t s02)
{
    const uint32_t t0 = now(s);
    uint32_t clear_at = 0;
    int clear = 0, rc;
    s->fault = 1;
    for (;;) {
        if (s02 & 0x0C) {
            const qd_state_t st = (s02 & 0x08) ? QDS_JAM : QDS_COVER;
            if (!s->have_st || s->st != st || clear) {
                state(s, st);
                if ((rc = lights(s, qd_lights(st, 0))) || (rc = flush(s))) return rc;
            }
            clear = 0;
        } else if (!clear) {
            clear = 1;
            clear_at = now(s);
        } else if (since(s, clear_at) >= CLEAR_MS) {
            int a = q(s, 0x83, 0x01);
            if (a < 0) return a;
            if (!(a & 0x20)) break;
        }
        if (stopping(s)) return stop_now(s);
        if (since(s, t0) >= s->stall) return QD_E_STALL;
        s->o.sleep_ms(s->o.ctx, POLL_MS);
        int b = q(s, 0x83, 0x02);
        if (b < 0) return b;
        s02 = (uint8_t)b;
    }
    s->fault = 0;
    return 0;
}

// A jam or the cover stopped the page printing: stop, wait until it is gone, then Apple's
// recovery up to its last 8A 01, and 83 02 once more: back again, wait again.
static int recover_to_print(qd_session_t *s, uint8_t s02)
{
    int rc = cmd(s, 0x8A, 0x00);
    if (rc) return rc;
    for (;;) {
        if ((rc = wait_clear(s, s02))) return rc;
        if ((rc = cmd(s, 0x81, 0x00)) || (rc = cmd(s, 0x84, 0x08)) || (rc = flush(s))) return rc;
        s->o.sleep_ms(s->o.ctx, RESET_MS);
        if ((rc = lights(s, 0x01))) return rc;
        static const uint8_t again[4] = { 0x40, 0x3E, 0x00, 0x5D };
        for (int i = 0; i < 4; i++)
            if ((rc = q(s, i == 2 ? 0x82 : 0x83, again[i])) < 0) return rc;
        if ((rc = q(s, 0x83, source(s))) < 0) return rc;
        if ((rc = q(s, 0x83, 0x02)) < 0) return rc;
        if (!(rc & 0x0C)) return 0;
        s02 = (uint8_t)rc;
    }
}

// The printer's own recovery, with no copy to send again: its 81 00 and 8A 01 print again the
// page at the start of the printer's memory. 8A 01 goes only with 83 02 clear: sent into a
// jam, the printer keeps it and feeds when the jam clears.
static int recover(qd_session_t *s, uint8_t s02)
{
    int rc = recover_to_print(s, s02);
    if (rc) return rc;
    if ((rc = cmd(s, 0x81, 0x00)) || (rc = cmd(s, 0x8A, 0x01)) || (rc = flush(s))) return rc;
    state(s, QDS_PRINTING);
    return 0;
}

static int resend(qd_session_t *s, uint8_t s02);
static int open_page(qd_session_t *s, int page, const qd_media_t *md);
static int row(qd_session_t *s, const uint8_t *codes, size_t n);
static int print(qd_session_t *s);

// With no copy: does the printer's own recovery print the page printing again? It prints
// from the start of its memory, where the first page since the last 81 00 is (bench A4c: that
// page again, and the page loaded behind it after; after a jam on page 2 it printed page 1).
static int jam_reprints(const qd_session_t *s)
{
    return s->p_first;
}

// Is there a copy of every page a jam now would need sent again?
static int copy_whole(const qd_session_t *s)
{
    if (!copying(s) || s->cc < 0) return 0;
    for (int i = s->cc; i < s->ncp; i++)
        if (!s->cp[i].complete) return 0;
    return 1;
}

// A jam or the cover stopped the page printing.
static int jammed(qd_session_t *s, uint8_t s02)
{
    s->fault = 1;
    if (stopping(s)) return stop_now(s);
    if (s->resending) {                    // a jam as the pages go again: from the top
        s->again_s02 = s02;
        return AGAIN;
    }
    if (copy_whole(s)) return resend(s, s02);
    if (!jam_reprints(s)) {
        // The job stops; the state shows the jam, as the status at rest will until it clears.
        const qd_state_t st = (s02 & 0x08) ? QDS_JAM : QDS_COVER;
        int rc = cmd(s, 0x8A, 0x00);
        state(s, st);
        if (!rc) rc = lights(s, qd_lights(st, 0));
        if (!rc) rc = flush(s);
        event(s, QD_EV_LOST, s->p_no, 0);
        return rc ? rc : QD_E_FAULT;
    }
    int rc = recover(s, s02);
    if (rc) return rc;
    s->recoveries++;
    s->rewound = 1;
    s->p_at = now(s);
    event(s, QD_EV_REPRINT, s->p_no, 0);
    return 0;
}

// No paper for the page printing: it waits in the printer for more.
static int out_of_paper(qd_session_t *s)
{
    if (stopping(s)) return stop_now(s);
    int rc;
    if ((rc = cmd(s, 0x8A, 0x00)) || (rc = wait_paper(s))) return rc;
    if ((rc = q(s, 0x83, 0x02)) < 0) return rc;
    if (rc & 0x0C) return jammed(s, (uint8_t)rc);      // no 8A 01 into a jam
    if ((rc = lights(s, 0x01)) || (rc = cmd(s, 0x8A, 0x01)) || (rc = flush(s))) return rc;
    state(s, QDS_PRINTING);
    s->p_at = now(s);
    return 0;
}

static int engine_fault(qd_session_t *s)
{
    int rc;
    if ((rc = q(s, 0x83, 0x04)) < 0 || (rc = q(s, 0x83, 0x07)) < 0) return rc;
    state(s, QDS_ENGINE);
    return QD_E_FAULT;
}

// One question, one try: any answer ends a run of polls with none.
static int poll(qd_session_t *s, uint8_t c, uint8_t a)
{
    int v = ask(&s->o, c, a, 1);
    if (v >= 0) s->misses = 0;
    return v;
}

// The printer stops answering: two polls in a row with no reply.
static int missed(qd_session_t *s, int v)
{
    if (v != QD_E_OFFLINE) return fail(s, v);
    return ++s->misses >= 2 ? fail(s, QD_E_OFFLINE) : 0;
}

// An 8E reply that came after its question's wait was over: taken before the next question's
// drain throws it away, since its page done is said once. 8E's status has its top five bits
// set, which no 83 reply has.
static int late_8e(qd_session_t *s)
{
    const qd_ops_t *o = &s->o;
    uint8_t b;
    if (o->read(o->ctx, &b, 1, 0, 0) != 1) return QD_E_OFFLINE;
    if (b == 0x00 && o->read(o->ctx, &b, 1, 50, 0) != 1) return QD_E_OFFLINE;
    return (b & 0xF8) == 0xF8 ? b : QD_E_OFFLINE;
}

static void page_done(qd_session_t *s)
{
    s->printing = 0;
    s->p_bytes = 0;
    if (s->cc >= 0) {
        s->cp[s->cc].state = CP_DONE;
        s->cp[s->cc].done_at = now(s);
        s->cc = -1;
        copy_trim(s);
    }
    event(s, QD_EV_DONE, s->p_no, since(s, s->p_at));
}

// 8E 00 for the page printing, one try; after the printer's BREAK, which says the page is
// done, one more at once if that got nothing. 1 done, 0 not, else a QD_E_ (QD_E_OFFLINE: no
// answer; missed() counts it).
static int ask_done(qd_session_t *s, int brk)
{
    int e = s->late ? late_8e(s) : QD_E_OFFLINE;
    s->late = 0;
    for (int try = 0; e == QD_E_OFFLINE; try++) {
        e = ask(&s->o, 0x8E, 0x00, 1);
        if (e == QD_E_OFFLINE) e = late_8e(s);
        if (!brk || try) break;
    }
    if (e < 0) {
        s->late = e == QD_E_OFFLINE;
        return e;
    }
    s->misses = 0;
    s->e_bits |= (uint8_t)(e & 0x03);
    if (!(e & 0x04)) return 0;
    page_done(s);
    return 1;
}

// 83 01 and 83 02 with a page printing, and what they say is wrong with it: a jam or the
// cover (recovered, or the job stops), an engine fault, no paper. 0, or a QD_E_.
static int printing_status(qd_session_t *s)
{
    int s01 = poll(s, 0x83, 0x01);
    int s02 = s01 < 0 ? s01 : poll(s, 0x83, 0x02);
    if (s02 < 0) return missed(s, s02);
    if (s02 & 0x0C) return jammed(s, (uint8_t)s02);
    // No paper at the source also says a fault is stored: it waits below, as for paper.
    if ((s01 & 0x02) && !(s02 & QD_S02_NO_PAPER)) return engine_fault(s);
    if (!(s01 & 0x20)) {
        // Not moving: is the paper gone? Then the page waits in the printer for more.
        int a = poll(s, 0x83, 0x2A);
        if (a < 0) return missed(s, a);
        if (!(a & paper_bit(s))) return out_of_paper(s);
    }
    return 0;
}

// The page printing, to its done: a question every half second (at once at the printer's
// BREAK), through no paper, a jam and the cover, for up to 85 s from its 8A 01. A cancel does
// not end it while nothing is wrong: the sheet comes out (stopped mid-sheet, it comes out
// blank), and the caller stops the job after.
static int wait_done(qd_session_t *s)
{
    const qd_ops_t *o = &s->o;
    int rc;
    uint32_t poll_at = now(s) - POLL_MS;       // the first question at once
    for (;;) {
        if (!s->printing) return QD_OK;        // done as the pages went again after a jam
        // The next half second, or at once when the printer sends a BREAK.
        int brk = 0;
        while (!brk && since(s, poll_at) < POLL_MS) {
            if (o->brk && o->brk(o->ctx)) { brk = 1; break; }
            uint32_t left = POLL_MS - since(s, poll_at);
            o->sleep_ms(o->ctx, left < 50 ? left : 50);
        }
        poll_at = now(s);
        int e = ask_done(s, brk);
        if (e > 0) return QD_OK;
        if (e < 0 && (rc = missed(s, e))) return rc;
        if (since(s, s->p_at) >= DONE_MS) return QD_E_FAULT;
        if (e < 0) continue;
        const unsigned rec = s->recoveries;
        const uint32_t at = s->p_at;
        if ((rc = printing_status(s))) return rc;
        if (s->recoveries != rec || s->p_at != at) {
            poll_at = now(s);                  // printing again: its first question in 500 ms
            continue;
        }
        if ((rc = blink(s)) || (rc = flush(s))) return rc;
    }
}

// The job is to stop. A sheet printing with nothing wrong comes out first.
static int cancel(qd_session_t *s)
{
    if (s->printing) {
        s->stopping = 1;
        int rc = wait_done(s);
        if (rc) return rc;
    }
    return stop_now(s);
}

// The setup of a page with nothing printing: 81 00 starts the printer's memory over.
static int setup(qd_session_t *s, const qd_media_t *md)
{
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
    s->printed = 0;
    s->rewound = 0;
    return 0;
}

static int open_page(qd_session_t *s, int page, const qd_media_t *md)
{
    int v;
    if (s->printing && !s->loading && !s->rewound && md == s->p_md) {
        // The next page, behind the one printing: the offsets and the width are that page's
        // while it prints, and are this one's too. No answer here is the line's trouble, not
        // the page's: the state is left alone, and abort lets the sheet printing come out.
        if ((v = ask(&s->o, 0x83, source(s), 0)) < 0) {
            s->soft = v == QD_E_OFFLINE;
            return v;
        }
    } else {
        // Another sheet; or rows are in that will not print (this page's, when it goes again
        // in Apple's screen, or a page's that fit neither); or a recovery left the printer at
        // the start of its memory with a page behind. The page printing finishes, and 81 00
        // starts over.
        if (s->printing && (v = wait_done(s))) return v;
        if ((v = setup(s, md))) return v;
    }
    if ((v = flush(s))) return v;
    copy_open(s, page, md, s->loading);
    s->loading = 1;
    s->l_no = page;
    s->l_md = md;
    s->l_bytes = 0;
    s->rows = 0;
    s->e_bits = 0;
    s->blink_at = s->poll_at = now(s);
    return QD_OK;
}

// A recovery's 81 00 took the printer back to the start of its memory while this page was
// loading. With rows of it already in, the rest would land at the start, not behind them:
// the page is lost. With none in, it starts the memory over once the page printing is done.
static int after_recovery(qd_session_t *s)
{
    if (s->rows) {
        event(s, QD_EV_LOST, s->l_no, 0);
        return QD_E_FAULT;
    }
    int rc = s->printing ? wait_done(s) : 0;
    if (!rc) rc = setup(s, s->l_md);
    if (!rc) rc = flush(s);
    return rc;
}

// A row would take the page printing and this one past the printer's memory: the page
// printing finishes first, and its room is free.
static int make_room(qd_session_t *s)
{
    const uint32_t t0 = now(s);
    int rc = flush(s);
    if (!rc) rc = wait_done(s);
    if (rc) return rc;
    event(s, QD_EV_ROOM, s->l_no, since(s, t0));
    return 0;
}

// Between rows, at the printer's BREAK or every 500 ms: is the page printing done, and is
// anything wrong? One try a question. Whatever is wrong is seen to here, before another row
// goes: the printer was never tried taking rows with a jam or its cover open.
static int watch(qd_session_t *s, int brk)
{
    int rc = flush(s);
    if (rc) return rc;
    s->poll_at = now(s);
    if (s->printing) {
        int e = ask_done(s, brk);
        if (e < 0) return missed(s, e);
        if (e > 0) return 0;
        return printing_status(s);
    }
    // Nothing printing: a jam or the cover holds the rows until it is gone; paper is the
    // print's to wait for.
    int s01 = poll(s, 0x83, 0x01);
    int s02 = s01 < 0 ? s01 : poll(s, 0x83, 0x02);
    if (s02 < 0) return missed(s, s02);
    if (s02 & 0x0C) {
        if (stopping(s)) {
            s->fault = 1;
            return stop_now(s);
        }
        if ((rc = wait_clear(s, (uint8_t)s02))) return rc;
        state(s, QDS_PRINTING);
        if ((rc = lights(s, 0x01))) return rc;
        return flush(s);
    }
    if ((s01 & 0x02) && !(s02 & QD_S02_NO_PAPER)) return engine_fault(s);
    return 0;
}

static int row(qd_session_t *s, const uint8_t *codes, size_t n)
{
    if (!(s->rows & 31) && cancelled(s)) return cancel(s);
    const unsigned rec = s->recoveries;
    int rc = 0;
    if (s->printing && s->p_bytes + s->l_bytes + n > s->room) {
        rc = make_room(s);
    } else {
        int brk = s->o.brk && s->o.brk(s->o.ctx);
        if (brk || since(s, s->poll_at) >= POLL_MS) rc = watch(s, brk);
    }
    if (!rc && s->recoveries != rec) rc = after_recovery(s);
    if (rc) return rc;
    size_t len = qd_row_packet(codes, n, s->pkt);
    if (!len) return QD_E_FAULT;
    s->wrote = 1;
    rc = s->o.put(s->o.ctx, s->pkt, len);
    if (rc) return fail(s, rc);
    s->rows++;
    s->l_bytes += (uint32_t)n;
    copy_row(s, codes, n);
    return blink(s);
}

static int print(qd_session_t *s)
{
    int rc = flush(s);
    if (rc) return rc;
    // The page before this one comes out first.
    if (s->printing && (rc = wait_done(s))) return rc;
    if (cancelled(s)) return stop_now(s);
    // Did the page arrive whole? Memory full in either, or 8E's error bit, now or at any 8E
    // since its first row: it never prints.
    int e = q(s, 0x8E, 0x00);
    if (e < 0) return e;
    int c = q(s, 0x82, 0x00);
    if (c < 0) return c;
    if (((e | s->e_bits) & 0x03) || (c & 0x01)) return QD_E_FAULT;
    // Paper, and no jam or cover: 8A 01 sent into one is kept and runs when it clears.
    for (;;) {
        if ((rc = wait_paper(s))) return rc;
        if ((rc = q(s, 0x83, 0x02)) < 0) return rc;
        if (!(rc & 0x0C)) break;
        if ((rc = wait_clear(s, (uint8_t)rc))) return rc;
    }
    if ((rc = lights(s, 0x01)) || (rc = cmd(s, 0x8B, 0x8B)) || (rc = cmd(s, 0x8B, 0x8B)) ||
        (rc = cmd(s, 0x8A, 0x01)))
        return rc;
    state(s, QDS_PRINTING);
    if ((rc = flush(s))) return rc;
    s->blink_at = s->poll_at = s->p_at = now(s);
    s->printing = 1;
    s->p_no = s->l_no;
    s->p_md = s->l_md;
    s->p_bytes = s->l_bytes;
    s->p_first = s->printed++ == 0;
    s->loading = 0;
    s->l_bytes = 0;
    s->misses = 0;
    if (s->cl >= 0) {
        s->cp[s->cl].state = CP_PRINTING;
        s->cc = s->cl;
        s->cl = -1;
    }
    if (!s->resending) s->rip_open = 0;
    event(s, QD_EV_PRINT, s->p_no, 0);
    return QD_OK;
}

// A jam or the cover, with a copy of what the printer held: once it is gone, the pages that
// may not have come out go again from the copy, by the path they went by the first time
// (setup, rows, the check, the print sequence, each waiting for the done before it). They
// are the page whose done came less than 20 s before the jam (its sheet may be the one that
// jammed on its way out; it may come out twice), the page that was printing, and the rows so
// far of the page loading, which the rip then goes on with. The printer's own recovery is
// not relied on: after a jam on page 2 it printed page 1 (bench).
static int resend(qd_session_t *s, uint8_t s02)
{
    for (;;) {
        const uint32_t jam_at = now(s);
        s->resending = 0;
        int rc = recover_to_print(s, s02);
        if (rc) return rc;
        state(s, QDS_PRINTING);
        // The first to go: the page just done, if its sheet may still have been inside.
        int first = 0;
        while (first < s->ncp && s->cp[first].state == CP_DONE) first++;
        if (first > 0 && s->cp[first - 1].complete && jam_at - s->cp[first - 1].done_at < OUT_MS)
            first--;
        while (first-- > 0) copy_drop(s);
        s->printing = 0;
        s->loading = 0;
        s->p_bytes = 0;
        s->cc = -1;
        s->resending = 1;
        for (int i = 0; i < s->ncp && !rc; i++) {
            const cpage_t *c = &s->cp[i];
            const int whole = !(i == s->ncp - 1 && s->rip_open);
            event(s, QD_EV_RESENT, c->no, 0);
            if ((rc = open_page(s, c->no, c->md))) break;
            s->cl = i;
            s->cp[i].state = CP_LOADING;
            for (uint32_t at = 0; at < c->bytes && !rc;) {
                uint8_t head[2];
                ring_get(s, c->start + at, head, 2);
                const size_t n = (size_t)head[0] | (size_t)head[1] << 8;
                ring_get(s, c->start + at + 2, s->row_tmp, n);
                rc = row(s, s->row_tmp, n);
                at += (uint32_t)(2 + n);
            }
            if (!rc && whole) rc = print(s);
        }
        s->resending = 0;
        if (rc != AGAIN) {
            if (!rc) copy_trim(s);
            return rc;
        }
        s02 = s->again_s02;
    }
}

static int end(qd_session_t *s)
{
    int rc;
    if (s->printing && (rc = wait_done(s))) return rc;
    // A Cancel that came as the last page printed: its sheet is out, and the job ends
    // cancelled all the same.
    if (cancelled(s)) return stop_now(s);
    // Page done comes while the last sheet is still on its way out (83 01 bit 5, about 15 s
    // more on board 1): the job ends Printing then, and the status at rest says Ready once the
    // sheet is out, rather than Ready now and Printing again at the next status. One quick
    // try: a reply missed is Ready.
    int v = ask(&s->o, 0x83, 0x01, 1);
    rc = v == QD_E_OFFLINE || v >= 0 ? 0 : v;
    // A page from the manual slot leaves the printer taking paper from it: empty, it says it
    // has no paper, and it pulls in any sheet put there. The cassette again, as at rest.
    if (!rc && s->c.manual) {
        int a = ask(&s->o, 0x83, 0x4C, 1);
        if (a < 0 && a != QD_E_OFFLINE) rc = a;
    }
    if (!rc) rc = cmd(s, 0x81, 0x00);
    if (!rc) rc = lights(s, 0x01);
    if (!rc) rc = flush(s);
    s->closed = 1;
    if (!rc) state(s, v >= 0 && (v & 0x20) ? QDS_PRINTING : QDS_READY);
    return rc;
}

// ---- the calls: the first error one returns is kept, for abort

static int kept(qd_session_t *s, int rc)
{
    if (rc && !s->err) {
        s->err = rc;
        if (rc == QD_E_LINK) s->soft = 1;
    }
    return rc;
}

int qd_session_begin(qd_session_t *s) { return kept(s, begin(s)); }
int qd_session_open(qd_session_t *s, int page, const qd_media_t *md) { return kept(s, open_page(s, page, md)); }
int qd_session_row(qd_session_t *s, const uint8_t *codes, size_t n) { return kept(s, row(s, codes, n)); }
int qd_session_print(qd_session_t *s) { return kept(s, print(s)); }
int qd_session_end(qd_session_t *s) { return kept(s, end(s)); }

void qd_session_abort(qd_session_t *s)
{
    if (!s->wrote || s->closed) return;
    // No error of the session's own (the rip gave up, the upload was cut off), or one that
    // was the line's and not the printer's (a chunk read back wrong, the paper source's
    // answer missed): the sheet printing is sound. As a cancel, it comes out first.
    if (s->printing && !s->fault && (!s->err || s->soft)) {
        s->stopping = 1;
        if (wait_done(s) == QD_E_CANCEL) return;      // a fault came: stopped there
    }
    s->closed = 1;
    put_cmd(&s->o, 0x8A, 0x00);
    cassette_again(s);
    if (!s->fault) {                                  // (stop_now: nothing into a jam)
        put_cmd(&s->o, 0x81, 0x00);
        put_cmd(&s->o, 0x84, 0x01);
    }
    s->o.flush(s->o.ctx);
}
