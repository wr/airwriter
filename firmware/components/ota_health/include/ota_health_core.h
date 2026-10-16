// SPDX-License-Identifier: GPL-3.0-or-later
// The rollback health gate (W-1046), pure C. A new OTA image boots on trial
// (ESP-IDF's PENDING_VERIFY); this decides when it has proved itself, or that
// it never will and the bootloader should go back to the image before it.
// No ESP-IDF here; ota_health.c does the marking and the reboot.
#pragma once
#include <stdint.h>

// What a working AirWriter has up. Which of them count depends on whether the
// board has Wi-Fi credentials: with them it must be on the network and
// answering IPP; without them (a fresh board) its setup portal must be up.
enum {
    OH_STA_UP = 1u << 0,    // joined Wi-Fi, has an address
    OH_IPP_UP = 1u << 1,    // ipp_server listening
    OH_WEB_UP = 1u << 2,    // the web UI listening (it is also where an OTA lands)
    OH_AP_UP  = 1u << 3,    // setup access point up
};

typedef struct {
    uint32_t settle_ms;     // healthy this long without a break before it is believed
    uint32_t deadline_ms;   // from boot: not healthy by then, roll back
} oh_timing_t;

// The boot-time join gives up after 60 s and falls back to the setup AP, so
// 120 s covers a slow join with room to settle. 15 s of health catches an
// image that comes up and then dies on its first IPP or mDNS traffic.
#define OH_TIMING_DEFAULT { .settle_ms = 15000, .deadline_ms = 120000 }

typedef enum {
    OH_WAIT,                // nothing to do yet (or already decided)
    OH_MARK_VALID,          // keep this image: cancel the rollback
    OH_ROLLBACK,            // mark it invalid and reboot into the previous one
} oh_action_t;

typedef struct {
    oh_timing_t tm;
    uint32_t boot_ms;
    uint32_t need;          // the OH_* bits that make this board healthy
    uint32_t up;            // the OH_* bits true now
    uint32_t healthy_ms;    // when `up` last came to cover `need`
    uint8_t  healthy;
    uint8_t  decided;
} oh_gate_t;

void oh_init(oh_gate_t *g, const oh_timing_t *tm, int has_creds, uint32_t now_ms);
void oh_set(oh_gate_t *g, uint32_t bits, int on, uint32_t now_ms);
// Returns MARK_VALID or ROLLBACK exactly once, at the moment it is decided,
// and OH_WAIT on every other call.
oh_action_t oh_update(oh_gate_t *g, uint32_t now_ms);
