// SPDX-License-Identifier: GPL-3.0-or-later
//
// One EtherTalk link (W-1076), pure C: Ethernet frames and the clock in,
// frames and log lines out. It owns an ELAP-mode LocalTalk node (lt_node.c)
// and does what lies between that node and an Ethernet: AppleTalk address
// acquisition by AARP probe in the startup range (no router is ever assumed),
// answering AARP for its address, the AppleTalk-to-MAC map (from AARP and
// gleaned from every datagram received), and 802.2 SNAP framing. ethertalk.c
// runs one link per transport (Basilisk II's UDP tunnel, Infinite Mac's zone
// relay); the host tests in tools/rip drive it with a fake clock.
//
// The node hands datagrams over as LLAP-shaped buffers: a pseudo-LLAP header
// {dst node, src node, LLAP_LONG} and a long DDP datagram, both ways, so the
// node's frame path is the cable's.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "et_proto.h"
#include "lt_node.h"

#define ET_MAP_SLOTS     16
#define AARP_PROBES      10     // Inside AppleTalk: 10 probes,
#define AARP_PROBE_MS    200    // a fifth of a second apart
#define AARP_REQ_TRIES   3
#define AARP_REQ_MS      250

typedef struct {
    const char *object, *type;  // the NBP name, as the cable node's
    uint16_t    status;         // the Option Card status word to start with
    uint8_t     mac[ETH_ALEN];
    uint16_t    hint_net;       // last boot's address, tried first when it is in
    uint8_t     hint_node;      // the startup range (FF00-FFFE, 1-253); else random
    uint32_t    seed;           // nonzero
    uint32_t    idle_hold_ms;   // the node's idle hold (LT_IDLE_HOLD_MS)
    lt_name_t  *names;          // the node's room for Macintosh names, or NULL
    lt_reg_t   *regs;           // its room for registrations (W-1088), or NULL
} et_cfg_t;

typedef struct {
    void *ctx;
    // One Ethernet frame out, padded, without an FCS.
    void   (*send)(void *ctx, const uint8_t *frame, size_t n);
    void   (*log)(void *ctx, const char *line);
    // The printer, as lt_ops_t's job_* (one job at a time, across links).
    int    (*job_begin)(void *ctx);
    void   (*job_data)(void *ctx, const uint8_t *d, size_t n);
    size_t (*job_room)(void *ctx);
    void   (*job_end)(void *ctx, int normal);
    // The address is ours: keep it as next boot's hint. May be NULL.
    void   (*claimed)(void *ctx, uint16_t net, uint8_t node);
    // W-1082: the node's NBP answers to lt_node_lookup (lt_ops_t's found). May be NULL.
    void   (*found)(void *ctx, const nbp_tuple_t *t, const lt_route_t *r);
    // W-1088: lt_ops_t's pap_open, reg_status and reg_output. May be NULL.
    int    (*pap_open)(void *ctx, int reg, uint64_t now);
    size_t (*reg_status)(void *ctx, int reg, uint8_t *pstr, size_t cap);
    size_t (*reg_output)(void *ctx, int reg, uint8_t *buf, size_t max, int *eof, int again);
    // W-1098: lt_ops_t's name_check. May be NULL.
    void   (*name_check)(void *ctx, const nbp_tuple_t *t);
} et_ops_t;

typedef struct { uint16_t net; uint8_t node; uint8_t hw[ETH_ALEN]; uint64_t at; } et_map_t;
// A peer heard lately, from the map: its address, its MAC, how long ago (W-1079),
// and its Macintosh name as lt_node_name gives it, or NULL.
typedef struct {
    uint16_t net; uint8_t node; uint8_t hw[ETH_ALEN]; uint32_t ago_s; const uint8_t *name;
} et_peer_t;

typedef struct {
    et_ops_t  ops;
    lt_node_t node;             // LT_LINK_ELAP; its ops are this link's thunks
    uint8_t   mac[ETH_ALEN];
    uint16_t  net;              // the candidate, then ours
    uint8_t   nd;
    uint8_t   probes, clash;
    uint64_t  probe_at;
    uint64_t  now;              // the clock at the last call in, for the node's sends
    uint32_t  rng;
    et_map_t  map[ET_MAP_SLOTS];   // node 0 = a free slot
    uint8_t   pend[ELAP_MAX_FRAME];  // one datagram waiting on AARP, as the node sent it
    size_t    pend_len;
    uint8_t   pend_tries;
    uint64_t  pend_at;
    struct { uint32_t rx, tx, bad, own, aarp_in, aarp_out, unresolved; } st;
} et_link_t;

void et_link_init(et_link_t *l, const et_cfg_t *cfg, const et_ops_t *ops, uint64_t now);
// One Ethernet frame as the transport received it.
void et_link_rx(et_link_t *l, const uint8_t *frame, size_t n, uint64_t now);
// Timers: probes while acquiring, AARP retries, and the node's own.
void et_link_tick(et_link_t *l, uint64_t now);
// The MAC changed (the tunnel's is the board's IP): the map and any job are
// dropped, and the address is acquired again under the new MAC.
void et_link_set_mac(et_link_t *l, const uint8_t mac[ETH_ALEN], uint64_t now);
// The transport went away (relay closed, Wi-Fi lost): any job ends abandoned
// now, not when the PAP timers notice.
void et_link_down(et_link_t *l, uint64_t now);
// The peers heard in the last within_ms, newest first, one per MAC (its newest
// address), at most max of them, printers aside (lt_node_is_printer).
size_t et_link_peers(const et_link_t *l, uint64_t now, uint32_t within_ms, et_peer_t *out, size_t max);
