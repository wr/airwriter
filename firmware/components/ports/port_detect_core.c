// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_detect_core.h"

#include <string.h>

enum { JOB_PRINTER = 1, JOB_LT = 2 };

void pd_init(pd_t *d, int n, uint32_t now_ms)
{
    memset(d, 0, sizeof *d);
    d->n = n < 0 ? 0 : n > PD_SOCKS ? PD_SOCKS : n;
    for (int i = 0; i < PD_SOCKS; i++) {
        d->s[i].trial_from = -1;
        d->s[i].job_at = now_ms;
    }
}

static int since(uint32_t now, uint32_t then, uint32_t ms)
{
    return (uint32_t)(now - then) >= ms;
}

// Takes in what socket i shows; true for "heard a frame within PD_QUIET_MS".
static void take_in(pd_sock_t *s, const pd_in_t *in, uint32_t now)
{
    if (in->job != s->job) {               // changed from outside (its setting, the page)
        s->job = in->job;
        s->job_at = now;
        s->trial = 0;
        s->trial_from = -1;
        s->ready_since = 0;
        memset(s->junk_at, 0, sizeof s->junk_at);
    }
    if (in->ready) {
        if (!s->ready_since) s->ready_since = now ? now : 1;
    } else {
        s->ready_since = 0;
    }
    if (in->job == JOB_LT && in->frames) s->frame_at = now ? now : 1;
    if (in->job == JOB_PRINTER && in->junk) {
        memmove(&s->junk_at[1], &s->junk_at[0], sizeof s->junk_at - sizeof s->junk_at[0]);
        s->junk_at[0] = now ? now : 1;
    }
}

static int heard_lately(const pd_sock_t *s, uint32_t now)
{
    return s->frame_at && !since(now, s->frame_at, PD_QUIET_MS);
}

// A LocalTalk frame heard since the handshake went ready: the line is held by what sends them
// (a Mac on a direct cable), not a printer, until the cable comes out and the line drops.
static int ready_by_localtalk(const pd_sock_t *s)
{
    return s->ready_since && s->frame_at && (int32_t)(s->frame_at - s->ready_since) >= 0;
}

static int act(pd_act_t *a, int sock, uint8_t job, pd_why_t why)
{
    a->sock = sock;
    a->job = job;
    a->why = why;
    return 1;
}

int pd_tick(pd_t *d, const pd_in_t in[], uint32_t now, int idle, pd_act_t *a)
{
    *a = (pd_act_t){ -1, 0, PD_WHY_NONE };
    for (int i = 0; i < d->n; i++) take_in(&d->s[i], &in[i], now);
    if (!idle || d->n < 2) return 0;

    for (int i = 0; i < d->n; i++) {
        pd_sock_t *s = &d->s[i];
        const pd_in_t *x = &in[i];
        int other = 1 - i;

        // A trial: kept once a frame is heard; else, past its wait, undone.
        if (x->job == JOB_LT && s->trial) {
            if (s->frame_at) {                  // pd_applied cleared it as the trial began
                s->trial = 0;
                s->trial_from = -1;
            } else if (since(now, s->job_at, PD_TRIAL_MS)) {
                // LocalTalk back to the socket it came from (which frees this one), or this
                // socket back to the printer job.
                if (s->trial_from >= 0 && !in[s->trial_from].fixed && in[s->trial_from].can_lt)
                    return act(a, s->trial_from, JOB_LT, PD_WHY_NO_FRAME);
                if (!x->fixed) return act(a, i, JOB_PRINTER, PD_WHY_NO_FRAME);
            }
            continue;
        }
        if (x->fixed) continue;

        // A printer on a LocalTalk socket that has been quiet.
        if (x->job == JOB_LT && x->can_print && !heard_lately(s, now) &&
            since(now, s->job_at, PD_READY_MS)) {
            if (x->clocked) return act(a, i, JOB_PRINTER, PD_WHY_CLOCK);
            if (s->ready_since && since(now, s->ready_since, PD_READY_MS) && !ready_by_localtalk(s))
                return act(a, i, JOB_PRINTER, PD_WHY_HANDSHAKE);
        }

        // LocalTalk, perhaps, on a printer socket with no printer on it (its line down, or up
        // with nothing answering there, as a Mac on a direct cable may hold it): a trial, unless
        // the other socket's LocalTalk is in use or can't be given up.
        if (x->job == JOB_PRINTER && x->can_lt && (!x->ready || x->silent) && !x->clocked &&
            s->junk_at[PD_JUNK_N - 1] && !since(now, s->junk_at[PD_JUNK_N - 1], PD_JUNK_MS) &&
            (!s->backoff_until || (int32_t)(now - s->backoff_until) >= 0)) {
            int taken = in[other].job == JOB_LT;
            if (taken && (in[other].fixed || heard_lately(&d->s[other], now))) continue;
            return act(a, i, JOB_LT, PD_WHY_TRAFFIC);
        }
    }
    return 0;
}

void pd_applied(pd_t *d, const pd_act_t *a, uint32_t now)
{
    if (a->sock < 0 || a->sock >= d->n) return;
    pd_sock_t *s = &d->s[a->sock];
    int other = 1 - a->sock;
    if (a->why == PD_WHY_TRAFFIC) {
        int from = d->s[other].job == JOB_LT ? other : -1;
        s->job = JOB_LT;
        s->job_at = now;
        s->trial = 1;
        s->trial_from = (int8_t)from;
        s->frame_at = 0;
        memset(s->junk_at, 0, sizeof s->junk_at);
        if (from >= 0) {
            d->s[from].job = JOB_PRINTER;
            d->s[from].job_at = now;
        }
        return;
    }
    if (a->why == PD_WHY_NO_FRAME) {
        // Whichever socket had the trial waits before the next.
        int t = d->s[a->sock].trial ? a->sock : other;
        d->s[t].trial = 0;
        d->s[t].trial_from = -1;
        d->s[t].backoff_until = now + PD_BACKOFF_MS;
        if (!d->s[t].backoff_until) d->s[t].backoff_until = 1;
        memset(d->s[t].junk_at, 0, sizeof d->s[t].junk_at);
    }
    s->job = a->job;
    s->job_at = now;
    if (a->job == JOB_LT && d->s[other].job == JOB_LT) {
        d->s[other].job = JOB_PRINTER;
        d->s[other].job_at = now;
    }
}

const char *pd_why_name(pd_why_t w)
{
    switch (w) {
    case PD_WHY_HANDSHAKE: return "a printer's handshake";
    case PD_WHY_CLOCK:     return "a printer's clock";
    case PD_WHY_TRAFFIC:   return "LocalTalk traffic, on trial";
    case PD_WHY_NO_FRAME:  return "no LocalTalk frame on trial";
    default:               return "nothing";
    }
}
