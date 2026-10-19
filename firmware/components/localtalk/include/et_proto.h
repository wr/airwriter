// SPDX-License-Identifier: GPL-3.0-or-later
//
// EtherTalk (AppleTalk Phase 2 on Ethernet) packet layouts: the 802.3 + 802.2
// SNAP frame around a DDP datagram or an AARP packet, and AARP itself. Parse
// and build only, pure C, no state -- et_link.c holds the state and the host
// tests in tools/rip drive both. Inside AppleTalk, 2nd ed. (cited in
// docs/reference/README.md): ELAP and AARP ch. 2-3. Multi-byte fields are
// big-endian on the wire.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"

#define ETH_ALEN        6
#define ELAP_HDR        22      // dst 6, src 6, 802.3 length 2, LLC AA AA 03, SNAP 5
#define ELAP_MIN_FRAME  60      // frames are padded to this (no FCS)
#define ELAP_MAX_FRAME  (ELAP_HDR + DDP_LONG_HDR + DDP_MAX_DATA)   // 621
#define AARP_LEN        28

enum { ET_DDP = 1, ET_AARP = 2 };

extern const uint8_t ET_BCAST[ETH_ALEN];     // ff:ff:ff:ff:ff:ff
extern const uint8_t ET_AT_MCAST[ETH_ALEN];  // 09:00:07:ff:ff:ff, AppleTalk broadcast

typedef struct {
    const uint8_t *dst, *src;    // into the frame
    int            proto;        // ET_DDP or ET_AARP
    const uint8_t *data;         // after the SNAP header
    size_t         len;          // from the 802.3 length field, less LLC + SNAP
} elap_t;

// One frame into out, padded with zeros to ELAP_MIN_FRAME. Returns its
// length, or 0 when it does not fit cap or the payload is over a long DDP
// datagram's largest.
size_t elap_build(uint8_t *out, size_t cap, const uint8_t dst[ETH_ALEN],
                  const uint8_t src[ETH_ALEN], int proto, const uint8_t *data, size_t len);
// 0 with *e filled, or -1: not 802.3 + SNAP AppleTalk or AARP, truncated, a
// length field that disagrees with the frame, or over ELAP_MAX_FRAME (before
// padding is counted: a frame may carry padding past its length field).
int    elap_parse(const uint8_t *f, size_t n, elap_t *e);

enum { AARP_REQUEST = 1, AARP_RESPONSE = 2, AARP_PROBE = 3 };

typedef struct {
    uint16_t fn;
    uint8_t  src_hw[ETH_ALEN]; uint16_t src_net; uint8_t src_node;
    uint8_t  dst_hw[ETH_ALEN]; uint16_t dst_net; uint8_t dst_node;
} aarp_t;

// 0, or -1: not Ethernet/AppleTalk with 6- and 4-byte addresses, a function
// other than request, response or probe, or truncated.
int    aarp_parse(const uint8_t *p, size_t n, aarp_t *a);
size_t aarp_build(uint8_t out[AARP_LEN], const aarp_t *a);
