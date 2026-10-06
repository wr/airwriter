// SPDX-License-Identifier: GPL-3.0-or-later
// DIP bank decode + debounce, pure C. Bit i of on_bits = DIP(i+1) switched ON
// (the glue reads closed-to-GND as ON). The assignments below are the
// canonical firmware semantics -- the silkscreen only numbers the switches.
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t mode;         // DIP1-2: 0 AirPrint, 1 +EtherTalk, 2 +LocalTalk, 3 rsvd
    uint8_t dev_mode;     // DIP3
    uint8_t test_on_boot; // DIP4
    uint8_t dither_ovr;   // DIP5-6: 0 none, 1 floyd, 2 bayer, 3 threshold
    uint8_t spare;        // DIP7-8 raw
} dip_config_t;

void dip_decode(uint8_t on_bits, dip_config_t *out);  // bit i = DIP(i+1) ON

typedef struct { uint8_t s[3]; uint8_t n; } dip_deb_t; // 3-sample debouncer

// Caller seeds *out with 0xFF (an impossible sample) so the first stable read always reports.
int dip_stable(dip_deb_t *d, uint8_t sample, uint8_t *out); // 1 when settled+changed
