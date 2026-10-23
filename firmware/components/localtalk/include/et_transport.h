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

// A frame out the tunnel or the Wi-Fi link. No buffer free (sendto's ENOMEM,
// esp_wifi_internal_tx's ESP_ERR_NO_MEM) is only a burst Wi-Fi has not sent
// yet, and a dropped frame costs a PAP printer its 2 s retry (W-1086: a tenth
// of a LaserWriter job's replies on board 1), so it waits ET_SEND_WAIT_MS and
// tries again, ET_SEND_TRIES times in all; any other failure is final. 0 sent,
// -1 not; the waits it took in *retries.
enum { ET_SEND_OK, ET_SEND_BUSY, ET_SEND_FAILED };
#define ET_SEND_TRIES   5
#define ET_SEND_WAIT_MS 10
typedef int  (*et_send_fn)(void *ctx, const uint8_t *f, size_t n);
typedef void (*et_wait_fn)(uint32_t ms);
int    et_send_patiently(et_send_fn send, void *ctx, const uint8_t *f, size_t n, et_wait_fn wait,
                         int *retries);

// The board's zone MAC: B2 and the station MAC's rest. Infinite Mac's Basilisk
// II sends unicast only to MACs starting B2 (its emulators' own; ether_js.cpp
// GENERATED_ETHER_ADDR_PREFIX) and drops the rest, so a Mac could look the
// board up but never open a connection to it. B2 is locally administered.
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
// The board's own Wi-Fi (W-1078): a frame as the station interface handed it
// over, as the 802.3 + SNAP frame et_link reads. Macs send 802.3 + SNAP and it
// passes as it is, except that the Wi-Fi hands it over with an 802.3 length
// of 0, rebuilt from the DDP header (or AARP's 28); an access point may also
// turn AppleTalk into Ethernet II (0x809B DDP, 0x80F3 AARP), likewise rebuilt. Its length, or 0 for anything
// that is not AppleTalk, malformed, or too big.
size_t et_radio_normalize(const uint8_t *in, size_t n, uint8_t *out, size_t cap);

// 1 and the pong for a ping frame, from mac back to whoever sent it; else 0.
int    et_zone_pong(const uint8_t *frame, size_t n, const uint8_t mac[ETH_ALEN],
                    uint8_t pong[ET_PONG_LEN]);
