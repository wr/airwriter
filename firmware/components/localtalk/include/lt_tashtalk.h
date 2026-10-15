// SPDX-License-Identifier: GPL-3.0-or-later
//
// The host side of TashTalk's UART protocol (lampmerchant/tashtalk,
// documentation/protocol.md), pure C so the host tests can drive it.
//
// TashTalk -> host: every frame it hears, whatever its destination, FCS
// included. 0x00 escapes: 00 FF is a literal zero, 00 FD ends a frame, 00 FE
// a framing error, 00 FA an abort, 00 FC a bad CRC (only with CRC checking on).
//
// Host -> TashTalk: commands, no escaping.
//   0x00            no-op; a run of them resynchronises a command parser left
//                   mid-command (the host rebooted halfway through one)
//   0x01 frame FCS  transmit an LLAP frame. TashTalk does the LLAP access:
//                   carrier sense, backoff, and for a directed data frame the
//                   lapRTS/lapCTS handshake (a broadcast gets a broadcast RTS)
//   0x02 bitmap     32 bytes, bit n set = TashTalk answers lapENQ and lapRTS
//                   for node n; byte n/8, bit n%8, LSB first
//   0x03 features   one byte (2.1.0+): bit 7 computes the FCS of outbound
//                   frames (the two bytes must still be sent), bit 6 checks
//                   inbound ones. Both off here: the FCS is ours.
// TashTalk reports nothing about a transmission: after 32 failed attempts it
// drops the frame silently. It relays every frame on the bus except its own
// and its automatic ACK/CTS. Its CTS out (RA2, IO6) is low when it can take
// bytes; its queue is 128 bytes and CTS rises at half full.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"

#define TT_CMD_NOP      0x00
#define TT_CMD_SEND     0x01
#define TT_CMD_NODES    0x02
#define TT_CMD_FEATURES 0x03
#define TT_SYNC_LEN     1024    // NULs that flush any half-sent command

typedef enum {
    TT_FRAME = 0,   // a frame ended cleanly (its FCS not yet checked)
    TT_BAD_CRC,     // TashTalk says the CRC was bad
    TT_FRAMING,     // framing error
    TT_ABORT,       // abort sequence
    TT_UNKNOWN,     // an escape we do not know
    TT_OVERFLOW,    // longer than any LLAP frame; dropped
} tt_event_t;

typedef void (*tt_frame_fn)(void *ctx, tt_event_t ev, const uint8_t *f, size_t n);

typedef struct {
    uint8_t buf[LLAP_MAX + 2];   // a whole frame with its FCS
    size_t  len;
    uint8_t esc, over;
} tt_rx_t;

void tt_rx_init(tt_rx_t *r);
// Feed bytes from TashTalk; fn is called once per frame end or error.
void tt_rx_feed(tt_rx_t *r, const uint8_t *p, size_t n, tt_frame_fn fn, void *ctx);

// 0x01 + frame + its FCS into out. Returns the length, 0 if it does not fit.
size_t tt_send_cmd(uint8_t *out, size_t cap, const uint8_t *frame, size_t n);
// 0x02 + the bitmap answering for `node` alone (0: for nothing). 33 bytes.
size_t tt_nodes_cmd(uint8_t out[33], uint8_t node);
