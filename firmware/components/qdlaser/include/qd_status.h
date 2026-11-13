// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: a QuickDraw LaserWriter's state, read from its status replies, and how it shows: what
// the phone is told (as sw_status.h does for StyleWriters) and the three front lights the board
// sets with `84` (the printer never sets them itself). Pure C; tools/rip builds it on the host.
#pragma once
#include <stdint.h>
#include "qd_model.h"

// 83 02's cassette bits: 10 the cassette pulled out (board 1's printer, 2026-11-13; 83 01 then
// also says a fault is stored), 20 not in properly (Apple's driver).
enum { QD_S02_CASSETTE = 0x30 };

typedef enum { QDS_READY, QDS_PRINTING, QDS_NO_PAPER, QDS_NO_CASSETTE, QDS_MANUAL, QDS_JAM,
               QDS_COVER, QDS_ENGINE, QDS_OFFLINE } qd_state_t;

// The replies to 83 01, 83 02, 83 04, 83 07, 83 2A and 82 00, and (qd_ask_status) the question
// that went unanswered when the printer said nothing, command then argument (0x8301), else 0.
typedef struct { uint8_t s01, s02, s04, s07, s2a, c82; uint16_t miss; } qd_raw_t;

// The state, first match wins: a jam (83 02 bit 3), the cover (bit 2), the cassette (bits 4 and
// 5), an engine fault (83 01 bit 1; which one is in 83 04 and 83 07), no paper at the source asked for
// (manual: the slot, QDS_MANUAL; else the cassette, QDS_NO_PAPER), paper moving (83 01 bit 5),
// else ready. Offline is the caller's: a printer whose clock has gone answers nothing.
qd_state_t qd_state_from(const qd_spec_t *sp, const qd_raw_t *r, int manual);

// What IPP reports: a printer-state-reasons keyword and the printer-state-message. NULL and NULL
// for ready and printing; literals that live for the program (ipp_server compares reasons by
// address).
typedef struct { const char *reason, *message; } qd_ipp_t;
qd_ipp_t qd_state_ipp(qd_state_t st);

int qd_state_ready(qd_state_t st);    // QDS_READY or QDS_PRINTING
int qd_state_fault(qd_state_t st);    // anything someone has to see to: not ready, printing or offline

// 84's argument: bit 0 Ready (blinking by blink_on while printing), bit 1 Paper, bit 2 Jam.
uint8_t qd_lights(qd_state_t st, int blink_on);
