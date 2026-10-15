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
    uint8_t gamer;        // DIP7: Ready and Asleep drift through RGB instead of white
    uint8_t spare;        // DIP8 raw
} dip_config_t;

void dip_decode(uint8_t on_bits, dip_config_t *out);  // bit i = switch labelled i+1 ON

// Rev A: S1 pin n sits under the switch labelled 9-n (the part's printed
// numbers run opposite to its pins), so net DIPn is label 9-n. Turns bits in
// net order (bit i = net DIP(i+1)) into label order. Check S1's pin 1 on rev B.
uint8_t dip_physical(uint8_t net_bits);

typedef struct { uint8_t s[3]; uint8_t n; } dip_deb_t; // 3-sample debouncer

// Caller seeds *out with 0xFF (an impossible sample) so the first stable read always reports.
int dip_stable(dip_deb_t *d, uint8_t sample, uint8_t *out); // 1 when settled+changed
