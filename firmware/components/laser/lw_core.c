// SPDX-License-Identifier: GPL-3.0-or-later
#include "lw_core.h"
#include <string.h>

static uint64_t since(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }

static int same_name(const uint8_t *a, const uint8_t *b)
{
    return a[0] == b[0] && !memcmp(a + 1, b + 1, a[0]);
}

void lwc_init(lwc_t *c, uint64_t now)
{
    (void)now;
    memset(c, 0, sizeof *c);                          // nothing heard, looked or shared
    lw_status_map(NULL, &c->st);
}

// The shared printer's entry: the cable's if it is heard there, else the
// first heard. -1 when it has not been heard.
static int shared_idx(const lwc_t *c)
{
    if (!c->shared) return -1;
    int any = -1;
    for (int i = 0; i < LW_HEARD_MAX; i++) {
        if (!c->heard[i].used || !same_name(c->heard[i].name, c->share_name)) continue;
        if (c->heard[i].link == LW_LINK_CABLE) return i;
        if (any < 0) any = i;
    }
    return any;
}

static int card_open(const lwc_t *c, uint64_t now)
{
    return c->card_at && since(now, c->card_at) < LW_CARD_MS;
}

void lwc_card(lwc_t *c, uint64_t now)
{
    // Opened after a while closed: look now rather than at the next 30 s.
    if (!card_open(c, now)) c->looked = 0;
    c->card_at = now ? now : 1;
}

void lwc_share(lwc_t *c, const uint8_t *name, uint64_t now)
{
    (void)now;
    c->shared = name != NULL;
    memset(c->share_name, 0, sizeof c->share_name);
    if (name) memcpy(c->share_name, name, 1 + (size_t)(name[0] <= NBP_NAME_MAX ? name[0] : NBP_NAME_MAX));
    c->share_name[0] = name && name[0] > NBP_NAME_MAX ? NBP_NAME_MAX : c->share_name[0];
    c->online = c->queried = c->want_query = 0;
    c->status[0] = '\0';
    lw_status_map(NULL, &c->st);
    c->looked = 0;                                    // look at once
    int i = shared_idx(c);
    if (i >= 0) {                                     // already heard: answering now
        c->online = 1;
        c->answered = c->heard[i].heard;
        c->want_query = 1;
    }
}

static void shared_answered(lwc_t *c, uint64_t now)
{
    c->answered = now;
    if (!c->online) {
        c->online = 1;
        if (!c->queried) c->want_query = 1;
    }
}

void lwc_found(lwc_t *c, const nbp_tuple_t *t, uint8_t link, const lt_route_t *r, uint64_t now)
{
    lw_heard_t *h = NULL;
    for (int i = 0; i < LW_HEARD_MAX && !h; i++)
        if (c->heard[i].used && c->heard[i].link == link && same_name(c->heard[i].name, t->obj))
            h = &c->heard[i];
    for (int i = 0; i < LW_HEARD_MAX && !h; i++)
        if (!c->heard[i].used) h = &c->heard[i];
    if (!h) {                                         // full: the one quiet longest goes
        h = &c->heard[0];
        for (int i = 1; i < LW_HEARD_MAX; i++)
            if (c->heard[i].heard < h->heard) h = &c->heard[i];
        h->used = 0;
    }
    if (!h->used) {
        memset(h, 0, sizeof *h);
        h->used = 1;
        h->link = link;
        memcpy(h->name, t->obj, 1 + (size_t)t->obj[0]);
    }
    h->route = *r;
    h->heard = now;
    if (c->shared && shared_idx(c) == (int)(h - c->heard)) shared_answered(c, now);
}

void lwc_status(lwc_t *c, int idx, const uint8_t *p, uint64_t now)
{
    if (idx < 0 || idx >= LW_HEARD_MAX || !c->heard[idx].used) return;
    lw_heard_t *h = &c->heard[idx];
    nbp_utf8(h->status, sizeof h->status, p);          // MacRoman: a Mac's job name (review I4)
    h->heard = now;
    if (idx != shared_idx(c)) return;
    memcpy(c->status, h->status, sizeof c->status);
    lw_status_map(c->status, &c->st);
    shared_answered(c, now);
}

void lwc_queried(lwc_t *c, int ok, uint64_t now)
{
    c->queried = ok ? 1 : 0;
    if (!ok) c->query_at = now + LW_QUERY_RETRY_MS;
}

void lwc_job(lwc_t *c, int printing) { c->printing = printing ? 1 : 0; }

lwc_want_t lwc_tick(lwc_t *c, uint64_t now)
{
    lwc_want_t w = { 0, LWC_NONE, 0 };
    for (int i = 0; i < LW_HEARD_MAX; i++)
        if (c->heard[i].used && since(now, c->heard[i].heard) >= LW_FORGET_MS) c->heard[i].used = 0;

    int idx = shared_idx(c);
    if (c->shared && c->online && since(now, c->answered) >= LW_GONE_MS) {
        c->online = 0;                                // gone: query again when it is back
        c->queried = 0;
    }
    if ((c->shared || card_open(c, now)) && (!c->looked || since(now, c->looked) >= LW_LOOK_MS)) {
        w.look = 1;
        c->looked = now ? now : 1;
    }
    if (idx >= 0) {
        uint32_t every = c->printing ? LW_STATUS_JOB_MS : LW_STATUS_MS;
        lw_heard_t *h = &c->heard[idx];
        if (since(now, h->asked) >= every && since(now, h->heard) >= every) {
            w.status_idx = (uint8_t)idx;
            h->asked = now;
        }
    }
    if (w.status_idx == LWC_NONE && card_open(c, now)) {
        for (int i = 0; i < LW_HEARD_MAX; i++) {
            if (!c->heard[i].used || i == idx || since(now, c->heard[i].asked) < LW_LOOK_MS) continue;
            w.status_idx = (uint8_t)i;
            c->heard[i].asked = now;
            break;
        }
    }
    if (c->want_query && c->online && !c->printing && now >= c->query_at) {
        w.query = 1;
        c->want_query = 0;
    } else if (c->shared && c->online && !c->queried && !c->want_query && !c->printing &&
               c->query_at && now >= c->query_at) {
        c->want_query = 1;                            // a failed query, tried again
    }
    return w;
}

lwc_view_t lwc_view(const lwc_t *c, uint64_t now)
{
    (void)now;
    lwc_view_t v = { .shared = c->shared, .online = c->online, .idx = shared_idx(c),
                     .ipp_state = LWC_IDLE, .reason = "none", .message = "" };
    if (!c->shared) return v;
    if (!c->online) {
        v.ipp_state = LWC_STOPPED;
        v.reason = "offline-report";
        v.message = "The LaserWriter does not answer";
        return v;
    }
    v.reason = c->st.reason;
    v.busy = c->st.busy;
    // The printer's own words go with a fault, a warning or busy (review I3).
    int say = c->st.stopped || c->st.busy || strcmp(c->st.reason, "none");
    v.message = say ? c->status : "";
    if (c->st.stopped) v.ipp_state = LWC_STOPPED;
    else if (c->st.busy || c->printing) v.ipp_state = LWC_PROCESSING;
    return v;
}
