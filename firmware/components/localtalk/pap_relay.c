// SPDX-License-Identifier: GPL-3.0-or-later
// The PAP relay (W-1088); see pap_relay.h.
#include "pap_relay.h"
#include <string.h>

static void L(pap_relay_t *r) { if (r->lock) r->lock(r->lock_ctx); }
static void U(pap_relay_t *r) { if (r->unlock) r->unlock(r->lock_ctx); }

static size_t used(const pr_ring_t *g) { return (g->head + g->cap - g->tail) % g->cap; }
static size_t room(const pr_ring_t *g) { return g->cap - 1 - used(g); }

static size_t put(pr_ring_t *g, const uint8_t *d, size_t n)
{
    size_t k = 0;
    for (; k < n && room(g); k++) {
        g->buf[g->head] = d[k];
        g->head = (g->head + 1) % g->cap;
    }
    return k;
}

static size_t get(pr_ring_t *g, uint8_t *d, size_t max)
{
    size_t k = 0;
    for (; k < max && used(g); k++) {
        d[k] = g->buf[g->tail];
        g->tail = (g->tail + 1) % g->cap;
    }
    return k;
}

static void empty(pap_relay_t *r)
{
    r->m2p.head = r->m2p.tail = r->p2m.head = r->p2m.tail = 0;
    r->asked = r->mac_eof = r->mac_gone = r->mac_had = r->prn_open = 0;
    r->prn_ended = r->cancelled = r->aborted = 0;
    r->prn_eof = r->mac_mid = r->told = r->closed = 0;
    r->answered = 0;
    r->prn_end = -1;
    r->last_n = 0;
    r->last_eof = 0;
}

void pap_relay_init(pap_relay_t *r, uint8_t *m2p, uint8_t *p2m, size_t cap)
{
    memset(r, 0, sizeof *r);
    r->m2p = (pr_ring_t){ m2p, cap, 0, 0 };
    r->p2m = (pr_ring_t){ p2m, cap, 0, 0 };
    empty(r);
}

int pap_relay_mac_open(pap_relay_t *r, const lt_node_t *mac, int reg, uint64_t now)
{
    int busy = 1;
    L(r);
    switch (r->state) {
    case PR_RELAY_IDLE:                           // ask the printer; the Mac hears busy until then
        empty(r);
        r->to_printer = r->to_mac = r->jobs = 0;
        r->state = PR_RELAY_ASKING;
        r->mac = mac;
        r->reg = (uint8_t)reg;
        r->since = now;
        break;
    case PR_RELAY_OPEN:                           // the printer is ready: the Mac's job begins
        if (r->mac == mac && r->reg == reg) {
            r->state = PR_RELAY_BOTH;
            r->since = now;
            r->mac_had = 1;
            busy = 0;
        }
        break;
    default:
        break;
    }
    U(r);
    return busy;
}

int pap_relay_active(const pap_relay_t *r) { return r->state != PR_RELAY_IDLE; }

int pap_relay_tick(pap_relay_t *r, uint64_t now)
{
    int act = 0;
    L(r);
    switch (r->state) {
    case PR_RELAY_ASKING:
        if (!r->asked) {
            r->asked = 1;
            act |= PR_RELAY_DO_OPEN;
        } else if (r->prn_ended) {                // busy past its wait, refused, or silent
            act |= PR_RELAY_DO_DONE;
            r->state = PR_RELAY_IDLE;
        }
        break;
    case PR_RELAY_OPEN:
        if (!r->since) r->since = now ? now : 1;  // opened since the last tick
        if (r->prn_ended) {
            act |= PR_RELAY_DO_DONE;
            r->state = PR_RELAY_IDLE;
        } else if (now - r->since >= PR_RELAY_WAIT_MS) {
            r->closed = 1;                        // the Mac never came back: nothing was sent
            act |= PR_RELAY_DO_CLOSE;
            r->state = PR_RELAY_ENDING;
        }
        break;
    case PR_RELAY_BOTH: {
        int mac_open = r->mac->pap.open && r->mac->pap.reg == r->reg;
        // W-1095: between jobs the Mac's last EOF has gone to the printer and
        // the printer has answered it with its own.
        int between = !r->mac_mid && !r->mac_eof && r->answered >= r->jobs;
        // The Mac is done or gone: between jobs the printer's closes normally;
        // mid-job it is cancelled, which ends the printer's job too.
        if ((!mac_open || r->mac_gone) && !r->prn_ended && !r->cancelled && !r->closed) {
            if (between) {
                r->closed = 1;
                act |= PR_RELAY_DO_CLOSE;
            } else {
                r->cancelled = 1;
                act |= PR_RELAY_DO_CANCEL;
            }
        }
        // The printer's connection ended: the Mac's ends with it, a failure at
        // once, a normal end once the printer's words and EOF have reached it.
        if (r->prn_ended && mac_open && !r->aborted && (r->prn_end != PAPC_END_OK || r->told)) {
            r->aborted = 1;
            act |= PR_RELAY_DO_ABORT_MAC;
        }
        if (r->prn_ended && !mac_open) {
            act |= PR_RELAY_DO_DONE;
            r->state = PR_RELAY_IDLE;
            empty(r);
        }
        break;
    }
    case PR_RELAY_ENDING:
        if (r->prn_ended) {
            act |= PR_RELAY_DO_DONE;
            r->state = PR_RELAY_IDLE;
            empty(r);
        }
        break;
    default:
        break;
    }
    U(r);
    return act;
}

// ------------------------------------------------------------- the Mac's side

void pap_relay_mac_data(pap_relay_t *r, const uint8_t *d, size_t n)
{
    L(r);
    r->to_printer += (uint32_t)put(&r->m2p, d, n);
    if (n) r->mac_mid = 1;
    U(r);
}

size_t pap_relay_mac_room(pap_relay_t *r)
{
    L(r);
    size_t k = r->mac_eof ? 0 : room(&r->m2p);   // the next job waits for this one's EOF
    U(r);
    return k;
}

void pap_relay_mac_end(pap_relay_t *r, int normal)
{
    L(r);
    if (!normal) r->mac_gone = 1;
    U(r);
}

void pap_relay_mac_eof(pap_relay_t *r)
{
    L(r);
    r->mac_eof = 1;
    r->mac_mid = 0;
    r->jobs++;
    U(r);
}

size_t pap_relay_mac_output(pap_relay_t *r, uint8_t *buf, size_t max, int *eof, int again)
{
    L(r);
    size_t n;
    if (again) {
        n = r->last_n < max ? r->last_n : max;
        memcpy(buf, r->last, n);
        *eof = r->last_eof;
    } else {
        if (max > sizeof r->last) max = sizeof r->last;
        n = get(&r->p2m, buf, max);
        // The printer's EOF reaches the Mac once its words have: this job's
        // (W-1095), or the connection's when it ended normally.
        *eof = !used(&r->p2m) && (r->prn_eof || (r->prn_ended && r->prn_end == PAPC_END_OK));
        if (*eof) {
            r->prn_eof = 0;
            if (r->prn_ended) r->told = 1;
        }
        memcpy(r->last, buf, n);
        r->last_n = (uint16_t)n;
        r->last_eof = (uint8_t)*eof;
        r->to_mac += (uint32_t)n;
    }
    U(r);
    return n;
}

size_t pap_relay_status(pap_relay_t *r, uint8_t *pstr, size_t cap)
{
    L(r);
    size_t n = r->status[0] ? 1 + (size_t)r->status[0] : 0;
    if (n > cap) n = 0;
    memcpy(pstr, r->status, n);
    U(r);
    return n;
}

// ------------------------------------------------------------- the printer's side

size_t pap_relay_prn_read(pap_relay_t *r, uint8_t *buf, size_t max, int *eof)
{
    L(r);
    size_t n = get(&r->m2p, buf, max);
    *eof = r->mac_eof && !used(&r->m2p);
    if (*eof) r->mac_eof = 0;                     // passed on: the next read is the next job's
    U(r);
    return n;
}

void pap_relay_prn_output(pap_relay_t *r, const uint8_t *d, size_t n)
{
    L(r);
    put(&r->p2m, d, n);
    U(r);
}

void pap_relay_prn_status(pap_relay_t *r, const uint8_t *pstr)
{
    L(r);
    memcpy(r->status, pstr, 1 + (size_t)pstr[0]);
    U(r);
}

void pap_relay_prn_opened(pap_relay_t *r)
{
    L(r);
    r->prn_open = 1;
    if (r->state == PR_RELAY_ASKING) {
        r->state = PR_RELAY_OPEN;
        r->since = 0;                             // the next tick starts the Mac's wait
    }
    U(r);
}

void pap_relay_prn_eof(pap_relay_t *r)
{
    L(r);
    r->prn_eof = 1;
    r->answered++;
    U(r);
}

void pap_relay_prn_ended(pap_relay_t *r, papc_end_t how)
{
    L(r);
    r->prn_ended = 1;
    r->prn_end = (int8_t)how;
    U(r);
}
