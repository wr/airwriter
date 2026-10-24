// SPDX-License-Identifier: GPL-3.0-or-later
// One EtherTalk link; see et_link.h.
#include "et_link.h"
#include "pap_client.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void say(et_link_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(et_link_t *l, const char *fmt, ...)
{
    char line[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (l->ops.log) l->ops.log(l->ops.ctx, line);
}

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t rnd(et_link_t *l)
{
    uint32_t x = l->rng;            // xorshift32
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return l->rng = x;
}

static void new_candidate(et_link_t *l)
{
    uint16_t net;
    uint8_t nd;
    do {
        net = (uint16_t)(0xFF00 + rnd(l) % 0xFF);   // FF00..FFFE
        nd = (uint8_t)(1 + rnd(l) % 253);           // 1..253
    } while (net == l->net && nd == l->nd);
    l->net = net;
    l->nd = nd;
}

static void send_frame(et_link_t *l, const uint8_t dst[ETH_ALEN], int proto,
                       const uint8_t *data, size_t len)
{
    uint8_t f[ELAP_MAX_FRAME];
    size_t n = elap_build(f, sizeof f, dst, l->mac, proto, data, len);
    if (!n) return;
    l->ops.send(l->ops.ctx, f, n);
    l->st.tx++;
}

static void send_aarp(et_link_t *l, uint16_t fn, const uint8_t to[ETH_ALEN],
                      const uint8_t *their_hw, uint16_t their_net, uint8_t their_node)
{
    aarp_t a = { .fn = fn, .src_net = l->net, .src_node = l->nd,
                 .dst_net = their_net, .dst_node = their_node };
    memcpy(a.src_hw, l->mac, ETH_ALEN);
    if (their_hw) memcpy(a.dst_hw, their_hw, ETH_ALEN);
    uint8_t p[AARP_LEN];
    aarp_build(p, &a);
    send_frame(l, to, ET_AARP, p, sizeof p);
    l->st.aarp_out++;
}

// ------------------------------------------------------------- the map

static et_map_t *map_find(et_link_t *l, uint16_t net, uint8_t node)
{
    for (int i = 0; i < ET_MAP_SLOTS; i++)
        if (l->map[i].node && l->map[i].node == node && l->map[i].net == net) return &l->map[i];
    return NULL;
}

static void map_put(et_link_t *l, uint16_t net, uint8_t node, const uint8_t hw[ETH_ALEN])
{
    if (!node || node == LLAP_BCAST) return;
    et_map_t *m = map_find(l, net, node);
    for (int i = 0; !m && i < ET_MAP_SLOTS; i++)
        if (!l->map[i].node) m = &l->map[i];
    if (!m) {                                      // full: the oldest goes
        m = &l->map[0];
        for (int i = 1; i < ET_MAP_SLOTS; i++)
            if (l->map[i].at < m->at) m = &l->map[i];
    }
    m->net = net;
    m->node = node;
    memcpy(m->hw, hw, ETH_ALEN);
    m->at = l->now;
}

// The datagram waiting on AARP, if its destination is now known.
static void pend_flush(et_link_t *l)
{
    if (!l->pend_len) return;
    const et_map_t *m = map_find(l, be16(l->pend + 4), l->pend[8]);
    if (!m) return;
    send_frame(l, m->hw, ET_DDP, l->pend, l->pend_len);
    l->pend_len = 0;
}

// ------------------------------------------------------------- node thunks

static void t_send(void *ctx, const uint8_t *f, size_t n)
{
    et_link_t *l = ctx;
    if (n < LLAP_HDR + DDP_LONG_HDR || f[2] != LLAP_LONG) return;
    const uint8_t *dg = f + LLAP_HDR;
    size_t dl = n - LLAP_HDR;
    uint16_t net = be16(dg + 4);
    uint8_t node = dg[8];
    if (node == LLAP_BCAST) {
        send_frame(l, ET_AT_MCAST, ET_DDP, dg, dl);
        return;
    }
    const et_map_t *m = map_find(l, net, node);
    if (m) {
        send_frame(l, m->hw, ET_DDP, dg, dl);
        return;
    }
    // Unknown: hold it (a newer one replaces it; ATP retries cover the loss)
    // and ask who has the address.
    memcpy(l->pend, dg, dl);
    l->pend_len = dl;
    l->pend_tries = 1;
    l->pend_at = l->now + AARP_REQ_MS;
    l->st.unresolved++;
    send_aarp(l, AARP_REQUEST, ET_AT_MCAST, NULL, net, node);
}

static void t_log(void *ctx, const char *line)
{
    et_link_t *l = ctx;
    if (l->ops.log) l->ops.log(l->ops.ctx, line);
}
static int t_begin(void *ctx)
{
    et_link_t *l = ctx;
    return l->ops.job_begin ? l->ops.job_begin(l->ops.ctx) : 0;
}
static void t_data(void *ctx, const uint8_t *d, size_t n)
{
    et_link_t *l = ctx;
    if (l->ops.job_data) l->ops.job_data(l->ops.ctx, d, n);
}
static size_t t_room(void *ctx)
{
    et_link_t *l = ctx;
    return l->ops.job_room ? l->ops.job_room(l->ops.ctx) : (size_t)-1;
}
static void t_found(void *ctx, const nbp_tuple_t *t, const lt_route_t *r)
{
    et_link_t *l = ctx;
    if (l->ops.found) l->ops.found(l->ops.ctx, t, r);
}

static void t_end(void *ctx, int normal)
{
    et_link_t *l = ctx;
    if (l->ops.job_end) l->ops.job_end(l->ops.ctx, normal);
}

// ------------------------------------------------------------- the link

void et_link_init(et_link_t *l, const et_cfg_t *cfg, const et_ops_t *ops, uint64_t now)
{
    memset(l, 0, sizeof *l);
    l->ops = *ops;
    memcpy(l->mac, cfg->mac, ETH_ALEN);
    l->rng = cfg->seed ? cfg->seed : 0x2434u;
    l->now = now;
    lt_cfg_t nc = { cfg->object, cfg->type, cfg->status, 0, cfg->seed, cfg->names };
    lt_ops_t no = { l, t_send, NULL, t_log, t_begin, t_data, t_room, t_end, t_found, NULL, NULL };
    lt_node_init_elap(&l->node, &nc, &no, cfg->idle_hold_ms, now);
    if (cfg->hint_net >= 0xFF00 && cfg->hint_net <= 0xFFFE && cfg->hint_node >= 1 &&
        cfg->hint_node <= 253) {
        l->net = cfg->hint_net;
        l->nd = cfg->hint_node;
    } else {
        new_candidate(l);
    }
    l->probe_at = now;
}

static void on_aarp(et_link_t *l, const elap_t *e)
{
    aarp_t a;
    if (aarp_parse(e->data, e->len, &a)) {
        l->st.bad++;
        return;
    }
    l->st.aarp_in++;
    int src_is_mine = a.src_net == l->net && a.src_node == l->nd;
    int dst_is_mine = a.dst_net == l->net && a.dst_node == l->nd;
    if (l->node.phase != LT_ONLINE) {
        // Someone has the candidate, or is probing for it too.
        if (src_is_mine || (a.fn == AARP_PROBE && dst_is_mine)) l->clash = 1;
        return;
    }
    if (a.fn != AARP_PROBE) map_put(l, a.src_net, a.src_node, a.src_hw);
    if (a.fn != AARP_RESPONSE && dst_is_mine)
        send_aarp(l, AARP_RESPONSE, a.src_hw, a.src_hw, a.src_net, a.src_node);
    pend_flush(l);
}

void et_link_rx(et_link_t *l, const uint8_t *f, size_t n, uint64_t now)
{
    l->now = now;
    l->st.rx++;
    elap_t e;
    if (elap_parse(f, n, &e)) {
        l->st.bad++;
        return;
    }
    if (!memcmp(e.src, l->mac, ETH_ALEN)) {        // our own, looped back
        l->st.own++;
        return;
    }
    if (memcmp(e.dst, l->mac, ETH_ALEN) && memcmp(e.dst, ET_BCAST, ETH_ALEN) &&
        memcmp(e.dst, ET_AT_MCAST, ETH_ALEN))
        return;
    if (e.proto == ET_AARP) {
        on_aarp(l, &e);
        return;
    }
    if (e.len < DDP_LONG_HDR) {
        l->st.bad++;
        return;
    }
    uint16_t snet = be16(e.data + 6);
    uint8_t snode = e.data[9];
    if (l->node.phase != LT_ONLINE) {
        if (snet == l->net && snode == l->nd) l->clash = 1;
        return;
    }
    map_put(l, snet, snode, e.src);
    pend_flush(l);
    uint8_t buf[LLAP_HDR + DDP_LONG_HDR + DDP_MAX_DATA];
    if (e.len > sizeof buf - LLAP_HDR) {
        l->st.bad++;
        return;
    }
    buf[0] = e.data[8];
    buf[1] = snode;
    buf[2] = LLAP_LONG;
    memcpy(buf + LLAP_HDR, e.data, e.len);
    lt_node_rx(&l->node, buf, LLAP_HDR + e.len, now);
}

void et_link_tick(et_link_t *l, uint64_t now)
{
    l->now = now;
    if (l->node.phase == LT_ONLINE) {
        if (l->pend_len && now >= l->pend_at) {
            if (l->pend_tries >= AARP_REQ_TRIES) {
                l->pend_len = 0;                   // nobody has it
            } else {
                l->pend_tries++;
                l->pend_at = now + AARP_REQ_MS;
                send_aarp(l, AARP_REQUEST, ET_AT_MCAST, NULL, be16(l->pend + 4), l->pend[8]);
            }
        }
        lt_node_tick(&l->node, now);
        return;
    }
    if (l->clash) {
        uint16_t was_net = l->net;
        uint8_t was_nd = l->nd;
        new_candidate(l);
        l->clash = 0;
        l->probes = 0;
        l->probe_at = now;
        say(l, "%u.%u is taken; trying %u.%u", was_net, was_nd, l->net, l->nd);
    }
    if (now < l->probe_at) return;
    if (l->probes >= AARP_PROBES) {
        lt_node_set_address(&l->node, l->net, l->nd, now);
        if (l->ops.claimed) l->ops.claimed(l->ops.ctx, l->net, l->nd);
        return;
    }
    send_aarp(l, AARP_PROBE, ET_AT_MCAST, NULL, l->net, l->nd);
    l->probes++;
    l->probe_at = now + AARP_PROBE_MS;
}

void et_link_set_mac(et_link_t *l, const uint8_t mac[ETH_ALEN], uint64_t now)
{
    l->now = now;
    lt_node_set_address(&l->node, 0, 0, now);      // ends any job abandoned
    if (l->node.client) papc_link_down(l->node.client, now);
    memcpy(l->mac, mac, ETH_ALEN);
    memset(l->map, 0, sizeof l->map);
    l->pend_len = 0;
    l->probes = 0;
    l->clash = 0;
    l->probe_at = now;
}

size_t et_link_peers(const et_link_t *l, uint64_t now, uint32_t within_ms, et_peer_t *out, size_t max)
{
    et_peer_t all[ET_MAP_SLOTS];
    size_t k = 0;
    for (int i = 0; i < ET_MAP_SLOTS; i++) {
        const et_map_t *m = &l->map[i];
        if (!m->node || now - m->at > within_ms) continue;
        et_peer_t p = { m->net, m->node, {0}, (uint32_t)((now - m->at) / 1000),
                        lt_node_name(&l->node, m->net, m->node) };
        memcpy(p.hw, m->hw, ETH_ALEN);
        size_t j = k++;
        while (j > 0 && all[j - 1].ago_s > p.ago_s) { all[j] = all[j - 1]; j--; }   // newest first
        all[j] = p;
    }
    // One computer, one peer: a MAC heard under an older address too (an
    // emulator restarted, a Mac that took another node) keeps only its newest.
    size_t n = 0;
    for (size_t i = 0; i < k; i++) {
        int dup = 0;
        for (size_t j = 0; j < n && !dup; j++) dup = !memcmp(all[j].hw, all[i].hw, ETH_ALEN);
        if (!dup) all[n++] = all[i];
    }
    if (n > max) n = max;
    memcpy(out, all, n * sizeof *out);
    return n;
}

void et_link_down(et_link_t *l, uint64_t now)
{
    l->now = now;
    lt_node_pap_abort(&l->node, now);
    if (l->node.client) papc_link_down(l->node.client, now);   // W-1082
    l->pend_len = 0;
}
