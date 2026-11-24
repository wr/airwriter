// SPDX-License-Identifier: GPL-3.0-or-later
//
// The StyleWriter adapter role (W-1193): see sw_adapter.h. Written from the
// facts in lpstyl's at_printer_* and TailTalk's adapter role, read as
// references; no code of either.
#include "sw_adapter.h"
#include <stdio.h>
#include <string.h>

enum { JOB_NONE = 0, JOB_WAITING, JOB_PIPING };

static void say(sw_adapter_t *a, const char *line)
{
    if (a->ops.log) a->ops.log(a->ops.ctx, line);
}

static int sock_index(uint8_t sock)
{
    int i = (int)sock - SWA_SOCK(0);
    return i >= 0 && i < SWA_NAMES ? i : -1;
}

static int on_pipe(const sw_adapter_t *a, int c) { return a->job == JOB_PIPING && c == a->pipe; }

// ------------------------------------------------------------- the job

// The job is over. The request and the status text go with it.
static void job_over(sw_adapter_t *a, int clean, const char *how)
{
    char line[48];
    a->job = JOB_NONE;
    a->ctl = a->pipe = -1;
    a->called = a->clean = a->mid = a->held = a->held_byte = 0;
    a->job_name = a->job_id = 0;
    memset(a->req, 0, sizeof a->req);
    memset(a->status, 0, sizeof a->status);
    memset(&a->mac, 0, sizeof a->mac);
    snprintf(line, sizeof line, "adapter: job ended %s", how);
    say(a, line);
    if (a->ops.job_end) a->ops.job_end(a->ops.ctx, clean);
}

// Close a connection a Mac opened, ourselves: the engine does not report it.
static void drop(sw_adapter_t *a, int c)
{
    adsp_close(&a->adsp, c, a->now);
    memset(&a->conn[c], 0, sizeof a->conn[c]);
    if (c == a->ctl) a->ctl = -1;
}

// The pipe never came up: the Mac hears it on whatever it still has open.
static void no_pipe(sw_adapter_t *a)
{
    if (a->pipe >= 0) adsp_close(&a->adsp, a->pipe, a->now);
    if (a->ctl >= 0) drop(a, a->ctl);
    job_over(a, 0, "no pipe");
}

// The board, or the name going, ends the job.
static void stop(sw_adapter_t *a, const char *how)
{
    if (a->job == JOB_NONE) return;
    if (a->pipe >= 0) adsp_close(&a->adsp, a->pipe, a->now);
    job_over(a, 0, how);
}

// Open the pipe: a connection from the name's socket to the Mac's data
// socket. The engine asks for its window before it says which connection it
// is, so `opening` answers for it.
static void call_back(sw_adapter_t *a)
{
    if (a->job != JOB_WAITING || a->called) return;
    a->called = 1;
    a->opening = 1;
    int c = adsp_open(&a->adsp, (uint8_t)SWA_SOCK(a->job_name), &a->mac, a->now);
    a->opening = 0;
    if (c < 0) { no_pipe(a); return; }
    a->pipe = (int8_t)c;
}

// A connection is gone, by the engine's word or after our own close of it.
static void gone(sw_adapter_t *a, int c)
{
    if (a->job != JOB_NONE && c == a->pipe) {
        a->pipe = -1;
        if (a->job == JOB_WAITING) no_pipe(a);
        else if (a->clean) job_over(a, 1, "cleanly");
        else job_over(a, 0, "Mac gone");
        return;
    }
    int asked_here = a->job != JOB_NONE && c == a->ctl;
    memset(&a->conn[c], 0, sizeof a->conn[c]);
    if (asked_here) {
        a->ctl = -1;
        call_back(a);                           // the Mac has gone to wait on its data socket
    }
}

// The printer's bytes to the Mac, as far as the pipe's send buffer has room
// (some of it is kept for answers), and the window said again if it reopened.
static void service(sw_adapter_t *a)
{
    if (a->job != JOB_PIPING) return;
    for (;;) {
        uint8_t buf[128];
        size_t room = adsp_tx_free(&a->adsp, a->pipe);
        if (room <= SWA_ANSWER_ROOM || !a->ops.job_output) break;
        room -= SWA_ANSWER_ROOM;
        if (room > sizeof buf) room = sizeof buf;
        size_t n = a->ops.job_output(a->ops.ctx, buf, room);
        if (!n) break;
        if (n > room) n = room;
        adsp_write(&a->adsp, a->pipe, buf, n, a->now);
    }
    adsp_window(&a->adsp, a->pipe, a->now);
}

// ------------------------------------------------------------- attentions

// The answer, on the stream of the connection that asked. A Mac that has left
// so many unacknowledged that this one does not fit is not reading: 0, and
// the connection is closed.
static int answer(sw_adapter_t *a, int c, const uint8_t *d, size_t n)
{
    if (adsp_write(&a->adsp, c, d, n, a->now) == n) return 1;
    adsp_close(&a->adsp, c, a->now);
    gone(a, c);
    return 0;
}

static int board_busy(sw_adapter_t *a)
{
    return a->job != JOB_NONE || (a->ops.busy && a->ops.busy(a->ops.ctx));
}

// 000C and 000D name a job by the request that began it.
static int same_request(const sw_adapter_t *a, const uint8_t *body, size_t n)
{
    if (n < 5) return 0;
    if (n > SWA_REQ_LEN) n = SWA_REQ_LEN;
    if (memcmp(body, a->req, n)) return 0;
    for (size_t i = n; i < SWA_REQ_LEN; i++) if (a->req[i]) return 0;
    return 1;
}

// 000B. The result: 00 00 accepted, 00 01 busy, FF FF refused.
static uint16_t print_request(sw_adapter_t *a, int c, const uint8_t *body, size_t n)
{
    char line[48];
    int i = sock_index(adsp_sock(&a->adsp, c));
    const lt_route_t *from = adsp_peer(&a->adsp, c);
    if (n < 5 || !body[3] || i < 0 || !from || !a->names[i].id) return 0xFFFF;
    unsigned net = from->addr.net, node = from->addr.node;
    if (a->job != JOB_NONE || !a->ops.job_begin ||
        a->ops.job_begin(a->ops.ctx, a->names[i].id)) {
        snprintf(line, sizeof line, "adapter: busy to %u.%u", net, node);
        say(a, line);
        return 0x0001;
    }
    memset(a->req, 0, sizeof a->req);
    memcpy(a->req, body, n < SWA_REQ_LEN ? n : SWA_REQ_LEN);
    memset(a->status, 0, sizeof a->status);
    a->job = JOB_WAITING;
    a->job_name = (uint8_t)i;
    a->job_id = a->names[i].id;
    a->ctl = (int8_t)c;
    a->pipe = -1;
    a->called = a->clean = a->mid = a->held = 0;
    a->mac = *from;
    a->mac.addr.sock = body[3];                 // its data socket, at its own address
    a->accepted_at = a->now;
    snprintf(line, sizeof line, "adapter: print request from %u.%u", net, node);
    say(a, line);
    return 0x0000;
}

// 0009: a name to take at this connection's 0012. One that cannot be a name
// stages nothing, and unstages what was.
static void stage_name(sw_adapter_t *a, int c, const uint8_t *body, size_t n)
{
    swa_conn_t *k = &a->conn[c];
    if (!k->open) return;
    memset(k->staged, 0, sizeof k->staged);
    if (n < 1 || body[0] < 1 || body[0] > NBP_NAME_MAX || n < 1u + body[0]) return;
    if (memchr(body + 1, ':', body[0]) || memchr(body + 1, '@', body[0])) return;
    memcpy(k->staged, body, 1u + body[0]);
}

// 000A: the message ID and its text, as 0010 gives them back.
static void keep_status(sw_adapter_t *a, const uint8_t *body, size_t n)
{
    memset(a->status, 0, sizeof a->status);
    if (n > SWA_STATUS_LEN) n = SWA_STATUS_LEN;
    memcpy(a->status, body, n);
    if (n < 3) return;
    if (a->status[2] > n - 3) a->status[2] = (uint8_t)(n - 3);   // no longer than what came
}

static void e_attention(void *ctx, int c, uint16_t code, const uint8_t *body, size_t n)
{
    sw_adapter_t *a = ctx;
    uint8_t r[2] = { 0, 0 };
    int piped = on_pipe(a, c);

    switch (code) {
    case SWA_JOB_USER:                          // 72 bytes: zeros with no job
        answer(a, c, a->req, sizeof a->req);
        return;
    case SWA_JOB_STATUS:                        // 259 bytes: zeros with none
        answer(a, c, a->status, sizeof a->status);
        return;
    case SWA_FAST_CLOCK:                        // no fast mode: the Mac reads the low byte
        r[1] = 0xEF;
        break;
    case SWA_FAST_SERIAL:
        r[0] = 0xFF; r[1] = 0xEF;
        break;
    case SWA_DRAINED:                           // off the pipe nothing waits
        if (!piped || !a->ops.job_drained || a->ops.job_drained(a->ops.ctx)) r[0] = r[1] = 0xFF;
        break;
    case SWA_SET_NAME:
        stage_name(a, c, body, n);
        break;
    case SWA_SET_STATUS:
        if (piped) keep_status(a, body, n);
        break;
    case SWA_PRINT: {
        uint16_t res = print_request(a, c, body, n);
        r[0] = (uint8_t)(res >> 8); r[1] = (uint8_t)res;
        break;
    }
    case SWA_QUEUE_POS:
    case SWA_JOB_COUNT:
        r[1] = board_busy(a) ? 1 : 0;
        break;
    case SWA_WITHDRAW:                          // answered, then acted on
        if (!answer(a, c, r, 2)) return;
        if (a->job == JOB_WAITING && same_request(a, body, n)) {
            if (a->pipe >= 0) adsp_close(&a->adsp, a->pipe, a->now);
            job_over(a, 1, "cleanly");          // the Mac's own doing
        }
        return;
    case SWA_END:
        if (piped) {                            // the close that follows is the job's clean end
            r[0] = r[1] = 0xFF;
            a->clean = 1;
            break;
        }
        if (!answer(a, c, r, 2)) return;
        if (a->conn[c].open && a->conn[c].staged[0]) {
            uint8_t name[NBP_NAME_MAX + 1];
            int i = sock_index(adsp_sock(&a->adsp, c));
            memcpy(name, a->conn[c].staged, sizeof name);
            memset(a->conn[c].staged, 0, sizeof a->conn[c].staged);
            if (i >= 0 && a->names[i].id) {
                say(a, "adapter: rename asked");
                if (a->ops.rename) a->ops.rename(a->ops.ctx, a->names[i].id, name);
            }
        }
        if (a->job == JOB_WAITING && c == a->ctl) call_back(a);
        return;
    default:                                    // 0003, 0004, 0008 and anything unknown
        break;
    }
    answer(a, c, r, 2);
}

// ------------------------------------------------------------- the engine's calls

static void e_send(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n)
{
    sw_adapter_t *a = ctx;
    if (a->ops.send) a->ops.send(a->ops.ctx, to, src_sock, p, n);
}

// A Mac opens to a name's socket. The pipe's place is never given away.
static int e_accept(void *ctx, uint8_t local_sock, const lt_route_t *from)
{
    sw_adapter_t *a = ctx;
    int i = sock_index(local_sock), n = 0;
    (void)from;
    if (i < 0 || !a->names[i].id) return 0;
    for (int c = 0; c < ADSP_CONNS; c++) n += a->conn[c].open;
    return n < SWA_CONTROLS;
}

static void e_opened(void *ctx, int c)
{
    sw_adapter_t *a = ctx;
    if (a->job == JOB_WAITING && c == a->pipe) {
        a->job = JOB_PIPING;
        a->mid = a->held = 0;
        say(a, "adapter: pipe up");
        if (a->ops.job_up) a->ops.job_up(a->ops.ctx);
        return;
    }
    memset(&a->conn[c], 0, sizeof a->conn[c]);
    a->conn[c].open = 1;
}

static void e_closed(void *ctx, int c, adsp_end_t why)
{
    (void)why;
    gone(ctx, c);
}

// The Mac's stream to the printer as it is, but for a message that is the one
// byte 'x' or 'h': the switch to a fast mode this port does not follow. Such a
// byte is held until its message ends or goes on.
static void e_data(void *ctx, int c, const uint8_t *d, size_t n, int eom)
{
    sw_adapter_t *a = ctx;
    if (!on_pipe(a, c)) return;                 // a stream that is not the pipe's is dropped
    if (a->held) {
        a->held = 0;
        if (!n && eom) { a->mid = 0; return; }
        if (a->ops.job_data) a->ops.job_data(a->ops.ctx, &a->held_byte, 1);
    } else if (!a->mid && n == 1 && (d[0] == 'x' || d[0] == 'h')) {
        if (!eom) { a->held = 1; a->held_byte = d[0]; a->mid = 1; }
        return;
    }
    if (n && a->ops.job_data) a->ops.job_data(a->ops.ctx, d, n);
    a->mid = !eom;
}

// The pipe's window is the room in front of the printer, less a held byte
// that may yet go there. Other connections say what Apple's adapter says;
// their stream is dropped.
static size_t e_room(void *ctx, int c)
{
    sw_adapter_t *a = ctx;
    if (!a->opening && !(a->job != JOB_NONE && c == a->pipe)) return SWA_CTL_WINDOW;
    size_t r = a->ops.job_room ? a->ops.job_room(a->ops.ctx) : 0;
    return r > a->held ? r - a->held : 0;
}

// ------------------------------------------------------------- the owner's calls

void swa_init(sw_adapter_t *a, const swa_ops_t *ops, uint32_t seed)
{
    const adsp_ops_t e = { a, e_send, e_accept, e_opened, e_closed, e_attention, e_data, e_room };
    memset(a, 0, sizeof *a);
    a->ops = *ops;
    a->ctl = a->pipe = -1;
    adsp_init(&a->adsp, &e, seed);
}

void swa_reset(sw_adapter_t *a)
{
    adsp_reset(&a->adsp);
    memset(a->conn, 0, sizeof a->conn);
    a->pipe = a->ctl = -1;
    if (a->job != JOB_NONE) job_over(a, 0, "stopped");
}

void swa_offer(sw_adapter_t *a, const swa_name_t *names, int n, uint64_t now)
{
    a->now = now;
    for (int i = 0; i < SWA_NAMES; i++) {
        swa_name_t nw;
        memset(&nw, 0, sizeof nw);
        if (names && i < n) nw = names[i];
        if (nw.obj[0] > NBP_NAME_MAX) nw.obj[0] = NBP_NAME_MAX;
        if (nw.type[0] > NBP_NAME_MAX) nw.type[0] = NBP_NAME_MAX;
        if (nw.id != a->names[i].id) {
            if (a->job != JOB_NONE && a->job_name == i) stop(a, "withdrawn");
            for (int c = 0; c < ADSP_CONNS; c++)
                if (a->conn[c].open && adsp_sock(&a->adsp, c) == SWA_SOCK(i)) drop(a, c);
        }
        a->names[i] = nw;
    }
}

int swa_lookup(const sw_adapter_t *a, const nbp_tuple_t *q, int from, swa_name_t const **hit)
{
    for (int i = from < 0 ? 0 : from; i < SWA_NAMES; i++) {
        const swa_name_t *nm = &a->names[i];
        if (!nm->id || !nbp_match(q->obj, nm->obj) || !nbp_match(q->type, nm->type)) continue;
        if (hit) *hit = nm;
        return i;
    }
    return -1;
}

void swa_rx(sw_adapter_t *a, const ddp_t *d, uint64_t now)
{
    if (d->type != DDP_ADSP || sock_index(d->dst.sock) < 0) return;
    lt_route_t from = { d->src, d->llap_src, d->is_long };
    a->now = now;
    adsp_rx(&a->adsp, d->dst.sock, &from, d->data, d->len, now);
    service(a);
}

void swa_tick(sw_adapter_t *a, uint64_t now)
{
    a->now = now;
    if (a->job == JOB_WAITING) {
        if (now - a->accepted_at >= SWA_PIPE_MS) no_pipe(a);
        else if (now - a->accepted_at >= SWA_CALL_MS) call_back(a);
    }
    adsp_tick(&a->adsp, now);
    service(a);
}

void swa_stop(sw_adapter_t *a, uint64_t now)
{
    a->now = now;
    stop(a, "stopped");
}

int swa_job(const sw_adapter_t *a) { return a->job; }
