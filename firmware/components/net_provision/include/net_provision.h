// SPDX-License-Identifier: GPL-3.0-or-later
//
// How a user gets this printer onto their Wi-Fi.
//
// The device brings up its own access point and answers every DNS query with
// its own address, so joining that network opens the setup page by itself. No
// app, no pairing, and it works from an iPhone.
//
// SPEC.md 3.1 originally called for Improv over BLE. That cannot work here:
// Improv needs Web Bluetooth, which Safari on iOS does not implement and is
// not going to. A captive portal is what commercial printers fall back to when
// they cannot use Apple's WAC, and WAC needs an MFi licence and an
// authentication chip on the board.
#pragma once
#include "esp_err.h"
#include "esp_netif.h"

// Brings up the access point and the DNS responder. The caller starts the web
// UI with captive = 1 afterwards. `ap_name` is the SSID; the network is open,
// because a password nobody has been told is just a locked door.
esp_err_t net_provision_start(const char *ap_name);
void      net_provision_stop(void);

// True once the user has saved credentials through the portal. The device
// restarts on its own in that case, so this mostly matters for logging.
int       net_provision_done(void);
