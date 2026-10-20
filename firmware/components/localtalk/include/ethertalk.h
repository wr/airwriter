// SPDX-License-Identifier: GPL-3.0-or-later
//
// EtherTalk on the board: the printer the cable's LocalTalk node is, for Macs
// that network over Ethernet, reached three ways:
//
//   radio   EtherTalk on the home network itself, through the board's own
//           Wi-Fi (W-1078): real Macs on Ethernet, or on Wi-Fi through a
//           BlueSCSI's DaynaPORT. Frames come in through lwIP's
//           unknown-protocol hook (et_radio_hook.h), out through the Wi-Fi
//           driver, under the station's own MAC.
//   tunnel  Basilisk II's UDP tunnel ("udptunnel true", port 6066) on the
//           home network: the Macintosh Mini, Basilisk II on a desktop (W-1076).
//   zone    Infinite Mac's zone relay, a WebSocket to infinitemac.org, joined
//           only while a zone name is set (W-1076).
//
// One task owns the three links (et_link.c) and prints through the printer
// every AppleTalk link shares (at_printer.c). Settings live in NVS namespace
// "et": radio and tunnel (u8, on by default), zone_on (u8, Infinite Mac's
// switch) and zone (its name), and each link's last address. The zone name works like a password: it is in NVS and on the
// settings page, never in a log line or /api/et.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "at_printer.h"
#include "localtalk.h"

typedef struct {
    const char *object;          // NBP object: the queue name
    const char *type;            // NBP type the Chooser looks up
    uint16_t    tunnel_status;   // Option Card status words to start with
    uint16_t    zone_status;
    uint16_t    radio_status;
    uint32_t    ip;              // the station's IPv4 address, host order (0: none yet)
    localtalk_printer_t printer;
    void      (*on_event)(lt_event_t ev);   // LT_EV_LOOKUP and LT_EV_NEW_NODE; may be NULL
} ethertalk_cfg_t;

// Once, on a station with an address; later calls do nothing.
esp_err_t ethertalk_start(const ethertalk_cfg_t *cfg);
int       ethertalk_running(void);
// The station's address changed (host order): the tunnel's MAC follows it.
void      ethertalk_ip_changed(uint32_t ip);
// The station lost Wi-Fi: both links' jobs end abandoned at once, not when a
// PAP or WebSocket timer notices.
void      ethertalk_net_down(void);
void      ethertalk_set_status(uint16_t tunnel_status, uint16_t zone_status, uint16_t radio_status);
// Saves and applies at once. infinite turns Infinite Mac's relay on or off
// (NVS et/zone_on) and keeps the zone name either way; the relay runs only
// when it is on and the name is set. A NULL zone keeps the name; anything
// else must be "" or pass et_zone_name_ok (ESP_ERR_INVALID_ARG, nothing
// saved). Any switch < 0 keeps it.
esp_err_t ethertalk_apply(int tunnel, int radio, int infinite, const char *zone);
void      ethertalk_settings(int *tunnel, int *radio, int *infinite, char *zone, size_t cap);
// GET /api/et: each link's state, address, MAC and counters, the shared
// printer's, and the log. Never the zone name.
void      ethertalk_json(char *buf, size_t cap);
