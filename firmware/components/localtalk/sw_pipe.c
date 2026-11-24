// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pipe between a Mac's StyleWriter driver and the port (W-1193): see
// sw_pipe.h.
#include "sw_pipe.h"
#include <string.h>

static void lk(sw_pipe_t *p) { if (p->lock) p->lock(p->lock_ctx); }
static void ul(sw_pipe_t *p) { if (p->unlock) p->unlock(p->lock_ctx); }

void sw_pipe_init(sw_pipe_t *p, void (*lock)(void *ctx), void (*unlock)(void *ctx), void *ctx)
{
    memset(p, 0, sizeof *p);
    p->lock = lock;
    p->unlock = unlock;
    p->lock_ctx = ctx;
}

void sw_pipe_begin(sw_pipe_t *p)
{
    lk(p);
    p->d_head = p->d_len = p->u_head = p->u_len = 0;
    p->down_total = p->up_total = p->taken = p->wrote = p->reached = p->up_lost = 0;
    p->ended = p->clean = p->stop = p->failed = p->told = 0;
    p->cap = NULL;
    p->cap_max = p->cap_len = 0;
    ul(p);
}

void sw_pipe_capture(sw_pipe_t *p, uint8_t *buf, size_t max)
{
    lk(p);
    p->cap = buf;
    p->cap_max = buf ? max : 0;
    p->cap_len = 0;
    ul(p);
}

const uint8_t *sw_pipe_captured(const sw_pipe_t *p, size_t *len)
{
    *len = p->cap ? p->cap_len : 0;
    return *len ? p->cap : NULL;
}

// ---- The rings: a head and a length each, the lock held.

static void ring_in(uint8_t *ring, size_t size, size_t head, size_t len, const uint8_t *d, size_t n)
{
    size_t at = (head + len) % size, first = size - at;
    if (first > n) first = n;
    memcpy(ring + at, d, first);
    memcpy(ring, d + first, n - first);
}

static void ring_out(const uint8_t *ring, size_t size, size_t head, uint8_t *out, size_t n)
{
    size_t first = size - head;
    if (first > n) first = n;
    memcpy(out, ring + head, first);
    memcpy(out + first, ring, n - first);
}

// ---- The link's side

size_t sw_pipe_room(sw_pipe_t *p)
{
    lk(p);
    size_t r = p->ended ? 0 : SW_PIPE_DOWN - p->d_len;
    ul(p);
    return r;
}

void sw_pipe_put(sw_pipe_t *p, const uint8_t *d, size_t n)
{
    lk(p);
    size_t room = p->ended ? 0 : SW_PIPE_DOWN - p->d_len;
    if (n > room) n = room;                     // the adapter never sends more than room
    ring_in(p->down, SW_PIPE_DOWN, p->d_head, p->d_len, d, n);
    p->d_len += n;
    p->down_total += (uint32_t)n;
    ul(p);
}

size_t sw_pipe_output(sw_pipe_t *p, uint8_t *buf, size_t max)
{
    lk(p);
    size_t n = p->u_len < max ? p->u_len : max;
    ring_out(p->up, SW_PIPE_UP, p->u_head, buf, n);
    p->u_head = (p->u_head + n) % SW_PIPE_UP;
    p->u_len -= n;
    ul(p);
    return n;
}

int sw_pipe_drained(sw_pipe_t *p)
{
    lk(p);
    int d = !p->d_len && p->taken == p->wrote;
    ul(p);
    return d;
}

void sw_pipe_end(sw_pipe_t *p, int clean)
{
    lk(p);
    if (!p->ended) {
        p->ended = 1;
        p->clean = clean ? 1 : 0;
    }
    ul(p);
}

void sw_pipe_stop(sw_pipe_t *p)
{
    lk(p);
    p->stop = 1;
    ul(p);
}

void sw_pipe_fail(sw_pipe_t *p)
{
    lk(p);
    p->failed = 1;
    ul(p);
}

// ---- The pump's side

size_t sw_pipe_take(sw_pipe_t *p, uint8_t *buf, size_t max)
{
    lk(p);
    size_t n = p->d_len < max ? p->d_len : max;
    ring_out(p->down, SW_PIPE_DOWN, p->d_head, buf, n);
    p->d_head = (p->d_head + n) % SW_PIPE_DOWN;
    p->d_len -= n;
    p->taken += (uint32_t)n;
    if (p->cap && p->cap_len < p->cap_max) {
        size_t keep = p->cap_max - p->cap_len;
        if (keep > n) keep = n;
        memcpy(p->cap + p->cap_len, buf, keep);
        p->cap_len += keep;
    }
    ul(p);
    return n;
}

void sw_pipe_wrote(sw_pipe_t *p, size_t n)
{
    lk(p);
    p->wrote += (uint32_t)n;
    if (p->wrote > p->taken) p->wrote = p->taken;
    ul(p);
}

void sw_pipe_from_printer(sw_pipe_t *p, const uint8_t *d, size_t n)
{
    lk(p);
    size_t room = SW_PIPE_UP - p->u_len;
    if (n > room) {
        p->up_lost += (uint32_t)(n - room);
        n = room;
    }
    ring_in(p->up, SW_PIPE_UP, p->u_head, p->u_len, d, n);
    p->u_len += n;
    p->up_total += (uint32_t)n;
    ul(p);
}

sw_pump_t sw_pipe_pump(sw_pipe_t *p, const sw_pump_io_t *io, uint8_t *buf, size_t cap)
{
    lk(p);
    int ended = p->ended, clean = p->clean, halt = p->stop || p->failed, told = p->told;
    if (halt && !ended && !told) p->told = 1;
    ul(p);
    // The board's stop, or a port that failed: the adapter still has the job,
    // so its owner ends it there, once. If it had none to end, it is over.
    if (halt && !ended) return told ? SW_PUMP_UNCLEAN : SW_PUMP_STOP;
    if (ended && (!clean || halt)) return SW_PUMP_UNCLEAN;

    // Read before the take: every byte put before the end was seen is in the
    // ring, so a clean end drains them all.
    size_t n = sw_pipe_take(p, buf, cap);
    if (n) {
        lk(p);
        p->reached += (uint32_t)n;
        int last = !p->d_len;
        ul(p);
        if (io->write(io->ctx, buf, n) || (last && io->flush && io->flush(io->ctx))) {
            sw_pipe_fail(p);
            return SW_PUMP_GO;                  // the next round says how it ends
        }
        sw_pipe_wrote(p, n);
    }
    if (ended && sw_pipe_drained(p)) return SW_PUMP_CLEAN;

    lk(p);
    size_t room = SW_PIPE_UP - p->u_len;
    ul(p);
    if (room > cap) room = cap;
    int r = io->read(io->ctx, buf, room, !n);
    if (r > 0) sw_pipe_from_printer(p, buf, (size_t)r > room ? room : (size_t)r);
    return SW_PUMP_GO;
}

uint32_t sw_pipe_down_total(const sw_pipe_t *p) { return p->down_total; }
uint32_t sw_pipe_up_total(const sw_pipe_t *p)   { return p->up_total; }
uint32_t sw_pipe_reached(const sw_pipe_t *p)    { return p->reached; }
int      sw_pipe_stopped(const sw_pipe_t *p)    { return p->stop; }
int      sw_pipe_failed(const sw_pipe_t *p)     { return p->failed; }
