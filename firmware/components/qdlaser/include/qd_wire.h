// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: what goes down the wire to a QuickDraw LaserWriter. Every packet is BREAK, idle, then
// its bytes: a command (`cmd arg cmd^arg`), a value after an 83 (`(v << 1)` with odd parity), or
// a row (`0x5000 - n` big-endian, n bytes of run codes, the XOR of everything before). The link
// clocks a packet out as a line image, one bit per printer clock: BREAK (100 bits low), idle
// (120 marks), each byte 8N1 LSB first, then marks before the next BREAK. Pure C, no state.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { QD_BREAK_BITS = 100, QD_IDLE_BITS = 120, QD_GAP_BITS = 180, QD_ROW_MAX = 4096 };

// cmd, arg, cmd ^ arg into out; returns 3.
size_t  qd_cmd(uint8_t cmd, uint8_t arg, uint8_t out[3]);
// A value sent as a second 83 packet's argument: (v << 1) with odd parity in bit 0.
uint8_t qd_value(uint8_t v);
// An 83 reply the printer meant: odd parity over the byte, and bit 7 (refused) clear.
int     qd_reply_ok(uint8_t r);
// A row packet: 0x5000 - n big-endian, the codes, the XOR of all before. Returns n + 3, or 0 for
// n == 0 or n > QD_ROW_MAX. out holds n + 3.
size_t  qd_row_packet(const uint8_t *codes, size_t n, uint8_t *out);

// The line image: bit k is (buf[k >> 3] >> (k & 7)) & 1, 1 is mark. Bits are written, never
// ORed, so buf need not be cleared first.
// The bits a packet of nbytes takes with its marks after: 100 + 120 + 10 * nbytes + 180.
size_t  qd_bits_len(size_t nbytes);
// Packet pkt of n bytes at *pos, with QD_GAP_BITS marks after; *pos moves past them. 0, or -1
// with *pos unchanged when it does not fit in cap_bits.
int     qd_bits_put(uint8_t *buf, size_t cap_bits, size_t *pos, const uint8_t *pkt, size_t n);
// n marks at *pos, or as many as fit in cap_bits.
void    qd_bits_marks(uint8_t *buf, size_t cap_bits, size_t *pos, size_t n);
