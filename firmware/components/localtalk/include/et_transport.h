// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rules of the two ways EtherTalk frames reach the board (W-1076), pure C
// so the host tests can hold them to what the other end does:
//
// Basilisk II's UDP tunnel (macemu BasiliskII/src/ether.cpp, "udptunnel"):
// each Ethernet frame is one UDP datagram to port 6066. A Mac's MAC is "B2"
// (42 32) and its host's IPv4 address; a frame to such a MAC goes to that IP,
// one to the Ethernet broadcast or the AppleTalk multicast to
// 255.255.255.255, and one to any other MAC nowhere.
//
// Infinite Mac's zone relay (infinite-mac worker/ethernet-zone.ts): JSON
// text over a WebSocket to wss://infinitemac.org/zone/<zone>/websocket.
// {"type":"init","macAddress":"aa:bb:.."} registers the client's MAC; out,
// {"type":"send","destination":D,"packetArray":[..]} with D "AT" (AppleTalk
// multicast), "*" (broadcast) or a MAC; in, {"type":"receive",
// "packetArray":[..]}. The emulators ping one another with an Ethernet frame
// whose payload is "pingPING" and a 4-byte time, answered "pingPONG!".
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "et_proto.h"

#define ET_TUNNEL_PORT 6066
#define ET_ZONE_MAX    32
#define ET_PONG_LEN    27

// The board's tunnel MAC for its IPv4 address (host order).
void   et_tunnel_mac(uint32_t ip, uint8_t mac[ETH_ALEN]);
// Where a frame to dst goes: 1 and the IP (host order; 0xFFFFFFFF for
// broadcast), or 0 when it goes nowhere.
int    et_tunnel_dest(const uint8_t dst[ETH_ALEN], uint32_t *ip);

// The board's zone MAC: locally administered (0A) and the station MAC's rest.
void   et_zone_mac(const uint8_t base[ETH_ALEN], uint8_t mac[ETH_ALEN]);
// 1 for 1 to ET_ZONE_MAX of A-Z a-z 0-9 - _ . (what goes into the relay's
// URL as it is), else 0.
int    et_zone_name_ok(const char *zone);
// The JSON for init and send, NUL-terminated; its length, or 0 when it does
// not fit cap.
size_t et_zone_init_json(char *out, size_t cap, const uint8_t mac[ETH_ALEN]);
size_t et_zone_send_json(char *out, size_t cap, const uint8_t *frame, size_t n);
// One relay message (len bytes, not NUL-terminated): 1 for a "receive" with
// its packet in frame[0..*n), 0 for another type, -1 for anything malformed
// or a packet over cap.
int    et_zone_parse(const char *msg, size_t len, uint8_t *frame, size_t cap, size_t *n);
// 1 and the pong for a ping frame, from mac back to whoever sent it; else 0.
int    et_zone_pong(const uint8_t *frame, size_t n, const uint8_t mac[ETH_ALEN],
                    uint8_t pong[ET_PONG_LEN]);
