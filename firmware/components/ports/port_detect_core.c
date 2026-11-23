// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_detect_core.h"

#include <string.h>

enum { JOB_PRINTER = 1, JOB_LT = 2 };

void pd_init(pd_t *d, int n, uint32_t now_ms)
{
    memset(d, 0, sizeof *d);
    d->n = n < 0 ? 0 : n > PD_SOCKS ? PD_SOCKS : n;
    d->boot_at = now_ms;
    for (int i = 0; i < PD_SOCKS; i++) d->s[i].job_at = now_ms;
}

void pd_start(pd_t *d, int i, uint8_t job, int heard)
{
    if (i < 0 || i >= d->n) return;
    d->s[i].job = job;
    d->s[i].heard = d->s[i].heard_boot = job == JOB_LT && heard;
}

static int since(uint32_t now, uint32_t then, uint32_t ms)
{
    return (uint32_t)(now - then) >= ms;
}

// A LocalTalk frame heard since the handshake went ready: the line is held by what sends them
// (a Mac on a direct cable), not a printer, until the cable comes out and the line drops.
static int ready_by_localtalk(const pd_sock_t *s)
{
    return s->ready_since && s->frame_at && (int32_t)(s->frame_at - s->ready_since) >= 0;
}

// A printer on socket s, by what it shows: its handshake held ready by no Mac, or a clock.
static int printer_shows(const pd_sock_t *s, const pd_in_t *in)
{
    return in->clocked || (s->ready_since && !ready_by_localtalk(s));
}

// Takes in what socket i shows.
static void take_in(pd_sock_t *s, const pd_in_t *in, uint32_t now)
{
    if (in->job != s->job) {               // changed from outside (its setting, the page)
        s->job = in->job;
        s->job_at = now;
        s->trial = 0;
        s->ready_since = 0;
        s->heard = s->heard_boot = 0;
        memset(s->junk_at, 0, sizeof s->junk_at);
    }
    if (in->ready) {
        if (!s->ready_since) s->ready_since = now ? now : 1;
    } else {
        s->ready_since = 0;
    }
    if (in->job == JOB_LT && in->frames) {
        s->frame_at = now ? now : 1;
        s->heard = 1;
        s->heard_boot = 0;
    }
    // A printer plugged in where LocalTalk was heard: not a word more until it is heard again.
    if (in->job == JOB_LT && printer_shows(s, in)) s->heard = s->heard_boot = 0;
    if (in->job == JOB_PRINTER && in->junk) {
        memmove(&s->junk_at[1], &s->junk_at[0], sizeof s->junk_at - sizeof s->junk_at[0]);
        s->junk_at[0] = now ? now : 1;
    }
}

static int heard_lately(const pd_sock_t *s, uint32_t now)
{
    return s->frame_at && !since(now, s->frame_at, PD_QUIET_MS);
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

        // A trial: kept once a frame is heard; else, past its wait, back to the printer job.
        // W-1186: and back at the first check a printer shows on it, a clock or a handshake
        // that came up since the trial began (one up before it, with nothing answering, is
        // what a Mac on a direct cable holds, and may be why the trial began): a printer
        // switched on mid-trial does not wait the trial out with its jobs told busy.
        if (x->job == JOB_LT && s->trial) {
            if (s->frame_at) s->trial = 0;      // pd_applied cleared it as the trial began
            else if (x->fixed) { }
            else if (x->clocked) return act(a, i, JOB_PRINTER, PD_WHY_CLOCK);
            else if (s->ready_since && (int32_t)(s->ready_since - s->job_at) > 0)
                return act(a, i, JOB_PRINTER, PD_WHY_HANDSHAKE);
            else if (since(now, s->job_at, PD_TRIAL_MS))
                return act(a, i, JOB_PRINTER, PD_WHY_NO_FRAME);
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
        // with nothing answering there, as a Mac on a direct cable may hold it): a trial,
        // whatever the other socket does.
        if (x->job == JOB_PRINTER && x->can_lt && (!x->ready || x->silent) && !x->clocked &&
            s->junk_at[PD_JUNK_N - 1] && !since(now, s->junk_at[PD_JUNK_N - 1], PD_JUNK_MS) &&
            (!s->backoff_until || (int32_t)(now - s->backoff_until) >= 0))
            return act(a, i, JOB_LT, PD_WHY_TRAFFIC);
    }
    return 0;
}

void pd_applied(pd_t *d, const pd_act_t *a, uint32_t now)
{
    if (a->sock < 0 || a->sock >= d->n) return;
    pd_sock_t *s = &d->s[a->sock];
    if (a->why == PD_WHY_TRAFFIC) {
        s->job = JOB_LT;
        s->job_at = now;
        s->trial = 1;
        s->frame_at = 0;
        s->heard = s->heard_boot = 0;
        memset(s->junk_at, 0, sizeof s->junk_at);
        return;
    }
    if (a->why == PD_WHY_NO_FRAME) {         // the socket waits before its next trial
        s->backoff_until = now + PD_BACKOFF_MS;
        if (!s->backoff_until) s->backoff_until = 1;
        memset(s->junk_at, 0, sizeof s->junk_at);
    }
    s->trial = 0;
    s->job = a->job;
    s->job_at = now;
    s->heard = s->heard_boot = 0;
}

int pd_heard(const pd_t *d, int i)
{
    return i >= 0 && i < d->n && d->s[i].heard;
}

int pd_may_send(const pd_t *d, const pd_in_t in[], int i, uint32_t now)
{
    if (i < 0 || i >= d->n || in[i].job != JOB_LT) return 0;
    if (in[i].fixed) return 1;
    const pd_sock_t *s = &d->s[i];
    if (!s->heard || printer_shows(s, &in[i])) return 0;
    return !s->heard_boot || since(now, d->boot_at, PD_BOOT_MS);
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
