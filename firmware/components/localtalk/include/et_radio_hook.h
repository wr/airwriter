// SPDX-License-Identifier: GPL-3.0-or-later
//
// lwIP's unknown-protocol hook (W-1078), set on the lwip component by this
// component's CMakeLists through ESP_IDF_LWIP_HOOK_FILENAME. lwIP hands over
// every frame it does not know: 802.3 frames and Ethernet types other than IP
// and ARP. ethertalk.c keeps the AppleTalk ones for its radio link (and frees
// them, answering ERR_OK) and leaves the rest to lwIP to drop, as it always did.
#pragma once
#include "lwip/err.h"
struct pbuf;
struct netif;
err_t ethertalk_radio_input(struct pbuf *p, struct netif *netif);
#define LWIP_HOOK_UNKNOWN_ETH_PROTOCOL(p, netif) ethertalk_radio_input(p, netif)
