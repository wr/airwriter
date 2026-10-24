// SPDX-License-Identifier: GPL-3.0-or-later
#include "lw_core.h"
#include <string.h>

static uint64_t since(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }

static int same_name(const uint8_t *a, const uint8_t *b)
{
    return a[0] == b[0] && !memcmp(a + 1, b + 1, a[0]);
}

static int type_is(const uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    if (p[0] != n) return 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t a = p[1 + i], b = (uint8_t)s[i];
        if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 32);
        if (b >= 'a' && b <= 'z') b = (uint8_t)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

void lwc_init(lwc_t *c, uint64_t now)
{
    memset(c, 0, sizeof *c);                          // nothing heard, looked or served
    c->burst_from = now;
    c->look_due = 1;
}

void lwc_look_now(lwc_t *c, uint64_t now)
{
    c->burst_from = now;
    c->look_due = 1;
}

// The order a printer heard on two links is reached by: the cable, the
// network, the tunnel, the zone.
static int link_rank(uint8_t link)
{
    switch (link) {
    case LW_LINK_CABLE:  return 0;
    case LW_LINK_RADIO:  return 1;
    case LW_LINK_TUNNEL: return 2;
    default:             return 3;
    }
}

// The heard[] entry target ti rides, or -1 when it has not been heard.
static int heard_of(const lwc_t *c, int ti)
{
    const lwc_target_t *t = &c->t[ti];
    int best = -1;
    for (int i = 0; i < LW_HEARD_MAX; i++) {
        const lw_heard_t *h = &c->heard[i];
        if (!h->used || h->aw || h->kind != t->kind || !same_name(h->name, t->name)) continue;
        if (best < 0 || link_rank(h->link) < link_rank(c->heard[best].link)) best = i;
    }
    return best;
}

static void reset_target(lwc_target_t *t)
{
    t->online = t->queried = t->printing = t->fails = 0;
    t->answered = t->asked = t->query_at = 0;
    t->status[0] = '\0';
    lw_status_map(NULL, &t->st);
}

static void answered(lwc_t *c, int ti, uint64_t now)
{
    lwc_target_t *t = &c->t[ti];
    t->answered = now ? now : 1;
    t->online = 1;
}

void lwc_targets(lwc_t *c, const lwc_target_in_t *in, int n, uint64_t now)
{
    lwc_target_t *old = c->was;
    memcpy(old, c->t, sizeof c->was);
    memset(c->t, 0, sizeof c->t);
    for (int k = 0; k < n && k < LWC_TARGETS; k++) {
        lwc_target_t *t = &c->t[k];
        int kept = 0;
        for (int j = 0; j < LWC_TARGETS && !kept; j++) {
            if (!old[j].used || old[j].id != in[k].id || old[j].kind != in[k].kind ||
                !same_name(old[j].name, in[k].name))
                continue;
            *t = old[j];
            kept = 1;
        }
        if (kept) continue;
        t->used = 1;
        t->id = in[k].id;
        t->kind = in[k].kind;
        memcpy(t->name, in[k].name, 1 + (size_t)(in[k].name[0] <= NBP_NAME_MAX ? in[k].name[0]
                                                                               : NBP_NAME_MAX));
        if (t->name[0] > NBP_NAME_MAX) t->name[0] = NBP_NAME_MAX;
        reset_target(t);
        int h = heard_of(c, k);                       // already heard: answering now
        if (h >= 0 && since(now, c->heard[h].heard) < LW_GONE_MS) answered(c, k, c->heard[h].heard);
    }
    c->next = 0;
}

int lwc_target(const lwc_t *c, int id)
{
    for (int i = 0; i < LWC_TARGETS; i++) if (c->t[i].used && c->t[i].id == id) return i;
    return -1;
}

static int aw_node(const lwc_t *c, uint8_t link, const lt_route_t *r)
{
    for (int i = 0; i < LWC_AW_MAX; i++)
        if (c->aw[i].at && c->aw[i].link == link && c->aw[i].node == r->addr.node &&
            c->aw[i].net == r->addr.net)
            return 1;
    return 0;
}

static void aw_add(lwc_t *c, uint8_t link, const lt_route_t *r, uint64_t now)
{
    int slot = 0;
    for (int i = 0; i < LWC_AW_MAX; i++) {
        if (c->aw[i].at && c->aw[i].link == link && c->aw[i].node == r->addr.node &&
            c->aw[i].net == r->addr.net) { slot = i; break; }
        if (c->aw[i].at < c->aw[slot].at) slot = i;
    }
    c->aw[slot].link = link;
    c->aw[slot].node = r->addr.node;
    c->aw[slot].net = r->addr.net;
    c->aw[slot].at = now ? now : 1;
    for (int i = 0; i < LW_HEARD_MAX; i++)
        if (c->heard[i].used && aw_node(c, c->heard[i].link, &c->heard[i].route)) c->heard[i].aw = 1;
}

void lwc_found(lwc_t *c, const nbp_tuple_t *t, uint8_t link, const lt_route_t *r, uint64_t now)
{
    if (type_is(t->type, "AirWriter")) {
        aw_add(c, link, r, now);
        return;
    }
    uint8_t kind = type_is(t->type, "LaserWriter") ? LWC_KIND_LW
                 : type_is(t->type, "ImageWriter") ? LWC_KIND_IW : 0;
    if (!kind || !t->obj[0]) return;
    lw_heard_t *h = NULL;
    for (int i = 0; i < LW_HEARD_MAX && !h; i++)
        if (c->heard[i].used && c->heard[i].link == link && c->heard[i].kind == kind &&
            same_name(c->heard[i].name, t->obj))
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
        h->kind = kind;
        h->first = now ? now : 1;
        memcpy(h->name, t->obj, 1 + (size_t)(t->obj[0] <= NBP_NAME_MAX ? t->obj[0] : NBP_NAME_MAX));
        if (h->name[0] > NBP_NAME_MAX) h->name[0] = NBP_NAME_MAX;
    }
    h->route = *r;
    h->heard = now ? now : 1;
    h->aw = (uint8_t)aw_node(c, link, r);
    for (int ti = 0; ti < LWC_TARGETS; ti++)
        if (c->t[ti].used && heard_of(c, ti) == (int)(h - c->heard)) answered(c, ti, now);
}

void lwc_name_check(lwc_t *c, const nbp_tuple_t *t, uint8_t link, uint64_t now)
{
    uint8_t kind = type_is(t->type, "LaserWriter") ? LWC_KIND_LW
                 : type_is(t->type, "ImageWriter") ? LWC_KIND_IW : 0;
    if (!kind || !t->obj[0] || (t->obj[0] == 1 && t->obj[1] == '=')) return;
    for (int i = 0; i < LW_HEARD_MAX; i++) {
        const lw_heard_t *h = &c->heard[i];
        if (h->used && h->link == link && h->kind == kind && same_name(h->name, t->obj) &&
            since(now, h->heard) < LW_GONE_MS)
            return;                                   // up: a Mac about to print to it
    }
    c->check_look_at = now + LW_CHECK_SETTLE_MS;      // the next check puts it off again
}

void lwc_status(lwc_t *c, int ti, const uint8_t *p, uint64_t now)
{
    if (ti < 0 || ti >= LWC_TARGETS || !c->t[ti].used) return;
    lwc_target_t *t = &c->t[ti];
    nbp_utf8(t->status, sizeof t->status, p);         // MacRoman: a Mac's job name (review I4)
    lw_status_map(t->status, &t->st);
    int h = heard_of(c, ti);
    if (h >= 0) c->heard[h].heard = now ? now : 1;
    answered(c, ti, now);
}

void lwc_queried(lwc_t *c, int ti, int ok, uint64_t now)
{
    if (ti < 0 || ti >= LWC_TARGETS || !c->t[ti].used) return;
    lwc_target_t *t = &c->t[ti];
    if (ok) {
        t->queried = 1;
        t->fails = 0;
        return;
    }
    if (t->fails < 255) t->fails++;
    t->query_at = now + (t->fails < LW_QUERY_TRIES ? LW_QUERY_RETRY_MS : LW_LOOK_SLOW_MS);
}

void lwc_job(lwc_t *c, int ti, int printing)
{
    if (ti >= 0 && ti < LWC_TARGETS) c->t[ti].printing = printing ? 1 : 0;
}

lwc_want_t lwc_tick(lwc_t *c, uint64_t now)
{
    lwc_want_t w = { 0, LWC_NONE, LWC_NONE, LWC_NONE };
    for (int i = 0; i < LW_HEARD_MAX; i++)
        if (c->heard[i].used && since(now, c->heard[i].heard) >= LW_FORGET_MS) c->heard[i].used = 0;
    for (int i = 0; i < LWC_AW_MAX; i++)
        if (c->aw[i].at && since(now, c->aw[i].at) >= LW_FORGET_MS) c->aw[i].at = 0;

    if (c->check_look_at && now >= c->check_look_at) {
        c->check_look_at = 0;
        c->look_due = 1;
    }
    uint64_t every = since(now, c->burst_from) <= LW_LOOK_BURST_MS ? LW_LOOK_MS : LW_LOOK_SLOW_MS;
    if (c->look_due || since(now, c->looked) >= every) {
        w.look = 1;
        c->look_due = 0;
        c->looked = now;
    }

    for (int i = 0; i < LW_HEARD_MAX; i++) {
        lw_heard_t *h = &c->heard[i];
        if (!h->used || h->told || h->aw || since(now, h->first) < LW_TELL_MS) continue;
        h->told = 1;
        w.tell = (uint8_t)i;
        break;
    }

    for (int ti = 0; ti < LWC_TARGETS; ti++) {
        lwc_target_t *t = &c->t[ti];
        if (t->used && t->online && since(now, t->answered) >= LW_GONE_MS) {
            t->online = 0;                            // gone: query again when it is back
            t->queried = 0;
            t->fails = 0;
            t->query_at = 0;
        }
    }

    // Status, one printer a tick, each in turn.
    for (int k = 0; k < LWC_TARGETS; k++) {
        int ti = (c->next + k) % LWC_TARGETS;
        lwc_target_t *t = &c->t[ti];
        if (!t->used || heard_of(c, ti) < 0) continue;
        uint32_t ev = t->printing ? LW_STATUS_JOB_MS : LW_STATUS_MS;
        if (since(now, t->asked) < ev || since(now, t->answered) < ev) continue;
        t->asked = now ? now : 1;
        w.status_ti = (uint8_t)ti;
        c->next = (uint8_t)((ti + 1) % LWC_TARGETS);
        break;
    }

    for (int ti = 0; ti < LWC_TARGETS; ti++) {
        lwc_target_t *t = &c->t[ti];
        if (!t->used || t->kind != LWC_KIND_LW || !t->online || t->queried || t->printing ||
            now < t->query_at)
            continue;
        t->query_at = now + LW_QUERY_RETRY_MS;        // asked: lwc_queried says when next
        w.query_ti = (uint8_t)ti;
        break;
    }
    return w;
}

lwc_view_t lwc_view(const lwc_t *c, int ti, uint64_t now)
{
    (void)now;
    lwc_view_t v = { .online = 0, .idx = -1, .ipp_state = LWC_STOPPED,
                     .reason = "offline-report", .message = "The printer does not answer" };
    if (ti < 0 || ti >= LWC_TARGETS || !c->t[ti].used) return v;
    const lwc_target_t *t = &c->t[ti];
    v.idx = heard_of(c, ti);
    v.online = t->online;
    if (!t->online) return v;
    v.ipp_state = LWC_IDLE;
    v.reason = t->st.reason;
    v.busy = t->st.busy;
    // The printer's own words go with a fault, a warning or busy (review I3).
    int say = t->st.stopped || t->st.busy || strcmp(t->st.reason, "none");
    v.message = say ? t->status : "";
    if (t->st.stopped) v.ipp_state = LWC_STOPPED;
    else if (t->st.busy || t->printing) v.ipp_state = LWC_PROCESSING;
    return v;
}
