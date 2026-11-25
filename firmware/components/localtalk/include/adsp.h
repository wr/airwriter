// SPDX-License-Identifier: GPL-3.0-or-later
//
// ADSP, the AppleTalk Data Stream Protocol (W-1193, StyleWriters design,
// "Phase 2", "ADSP"): the connections a Mac's StyleWriter driver makes to
// Apple's StyleWriter network adapter, which the board stands in for. A
// connection engine with no knowledge of the node or the adapter: packets and
// a clock in, packets and events out. Pure C, no state of its own: the owner
// allocates an adsp_t (PSRAM on the board) and the host tests in tools/rip
// drive it with a fake clock.
//
// A packet is DDP type 7: a 13-byte header (the sender's connection ID, the
// sequence of its first byte, the next byte the sender expects, its receive
// window, a descriptor), then data. The four open packets carry 8 more bytes
// (version 0x0100, the other end's connection ID, the attention sequence
// expected). Layouts are Inside AppleTalk, 2nd ed., ch. 12; what real peers
// do with them is from lpstyl's adsp.c and TailTalk's engine, read as
// references.
//
// What it does:
// - Accepts an Open Request on any socket the owner says yes for, and opens
//   to a far socket (the adapter's call-back). Connections are told apart by
//   our socket, the far address and the far connection ID together. An open
//   of ours is answered, or denied, only by the network and node it went to
//   (any socket there).
// - Stream, in order only: a packet ahead is dropped, an overlap gives its
//   new bytes, bytes beyond the taker's room are not taken. End of message
//   takes one sequence number. An ack for every packet that asks, and one
//   unasked when the window opens (adsp_window).
// - Sends with an ack asked on every packet, inside the far window, again
//   after ADSP_RESEND_MS and at once on Retransmit Advice. It never sends an
//   end of message.
// - Attention messages, received only: their own sequence, an ack (13 bytes)
//   for every one whether or not it asks, a repeat acked and not delivered.
//   They never touch the stream's acks or window.
// - Answers a probe with one ack; probes a silent far end itself and closes
//   on the fourth silent ADSP_PROBE_MS in a row.
// - Answers a Forward Reset; takes a Close Advice as final and sends one.
// Not built: sending attention messages, Forward Reset or Retransmit Advice;
// buffering out of order; Open Denial (an Open Request nobody wants gets
// silence).
//
// Every ops call may call back into the engine (adsp_write from attention,
// adsp_open from closed). An attention's ack has gone out before the call,
// so an answer written there follows it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"
#include "lt_node.h"

#define ADSP_HDR       13
#define ADSP_MAX_DATA  572
#define ADSP_CONNS     4          // per engine: the pipe and three others
#define ADSP_TX_CAP    640        // unacknowledged bytes kept per connection

#define ADSP_OPEN_PARAMS   8      // after the header of an open packet
#define ADSP_VERSION       0x0100
#define ADSP_ATTN_MAX      570    // an attention's body, after its 2-byte code
#define ADSP_OPEN_RETRY_MS 1000
#define ADSP_OPEN_RETRIES  5      // after the first request
#define ADSP_RESEND_MS     3000   // after the last transmission, with bytes unacknowledged
#define ADSP_RESENDS       5
#define ADSP_PROBE_MS      5000   // of silence from the far end
#define ADSP_PROBE_EXPIRIES 4     // in a row: three probes, then Close Advice

// The descriptor byte.
#define ADSP_CONTROL   0x80
#define ADSP_ACK_REQ   0x40
#define ADSP_EOM       0x20
#define ADSP_ATTENTION 0x10
#define ADSP_CODE_MASK 0x0F
enum {                            // control codes
    ADSP_PROBE_OR_ACK = 0, ADSP_OPEN_REQ, ADSP_OPEN_ACK, ADSP_OPEN_REQ_ACK, ADSP_OPEN_DENY,
    ADSP_CLOSE, ADSP_FWD_RESET, ADSP_FWD_RESET_ACK, ADSP_RETRANSMIT,
};

typedef struct {
    uint16_t id;                  // the sender's own connection ID
    uint32_t first;               // sequence of the first data byte (attention: its attention sequence)
    uint32_t next;                // next byte the sender expects (attention: next attention)
    uint16_t win;                 // bytes the sender takes from `next` on; 0 in attention packets
    uint8_t  desc;
} adsp_hdr_t;

// 0 with *h filled, or -1: under 13 bytes, or a control code above 8.
int  adsp_hdr_parse(const uint8_t *p, size_t n, adsp_hdr_t *h);
void adsp_hdr_build(uint8_t out[ADSP_HDR], const adsp_hdr_t *h);

typedef enum { ADSP_END_PEER, ADSP_END_SILENT, ADSP_END_RESEND, ADSP_END_OPEN } adsp_end_t;
typedef struct {
    void  *ctx;
    // One ADSP packet (header on) for DDP type 7, from our socket src_sock.
    void   (*send)(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n);
    // An Open Request reached local_sock: nonzero to accept.
    int    (*accept)(void *ctx, uint8_t local_sock, const lt_route_t *from);
    void   (*opened)(void *ctx, int c);                 // accepted, or our open was answered
    void   (*closed)(void *ctx, int c, adsp_end_t why); // never for adsp_close
    void   (*attention)(void *ctx, int c, uint16_t code, const uint8_t *body, size_t n);
    // Stream bytes in order; eom: this call ends a message (n may be 0). Never more than room.
    void   (*data)(void *ctx, int c, const uint8_t *d, size_t n, int eom);
    size_t (*room)(void *ctx, int c);                   // the receive window, in bytes
} adsp_ops_t;

// room is asked for a connection before opened names it: for the window in
// our Open Request (inside adsp_open) and in the Request+Ack that accepts.
// During closed, adsp_sock and adsp_peer still answer for c, and its place is
// free for an adsp_open made there.

typedef struct {
    uint8_t    state;             // free, dying (inside closed), opening, open
    uint8_t    local_sock;
    uint8_t    opener;            // we opened it
    uint8_t    fresh;             // open, and nothing has come or been acknowledged yet
    uint8_t    tries;             // Open Requests sent again
    uint8_t    resends, probes;
    uint16_t   id, far_id;
    lt_route_t peer;
    uint32_t   rx_seq;            // next stream byte expected
    uint32_t   rx_limit;          // the end of the window the far end was last told
    uint32_t   attn_rx;           // next attention expected
    uint32_t   tx_una;            // sequence of tx[0], the oldest byte unacknowledged
    uint32_t   tx_limit;          // the end of the far end's window
    uint16_t   tx_len, tx_sent;   // bytes kept; those of them sent
    uint64_t   heard;             // when the far end last sent anything
    uint64_t   timer;             // the last Open Request, or the last probe
    uint64_t   tx_at;             // the last data transmission
    uint8_t    tx[ADSP_TX_CAP];
} adsp_conn_t;

struct adsp {
    adsp_ops_t  ops;
    uint16_t    next_id;
    adsp_conn_t c[ADSP_CONNS];
};
typedef struct adsp adsp_t;       // defined in the header; the owner allocates it

// Connection IDs count on from seed, never 0, never one in use.
void   adsp_init(adsp_t *a, const adsp_ops_t *ops, uint32_t seed);
void   adsp_reset(adsp_t *a);                           // forget every connection, send nothing
// The data of a DDP type 7 datagram to local_sock.
void   adsp_rx(adsp_t *a, uint8_t local_sock, const lt_route_t *from,
               const uint8_t *p, size_t n, uint64_t now);
int    adsp_open(adsp_t *a, uint8_t local_sock, const lt_route_t *to, uint64_t now); // c, or -1
size_t adsp_write(adsp_t *a, int c, const uint8_t *d, size_t n, uint64_t now);       // bytes taken
size_t adsp_tx_free(const adsp_t *a, int c);
int    adsp_tx_idle(const adsp_t *a, int c);            // nothing unacknowledged
// Room changed. Said unasked when the window the far end was last told is
// used up and there is room, or has under ADSP_MAX_DATA left and there is that
// much more: a far end that waits for a whole packet's room hears at once.
void   adsp_window(adsp_t *a, int c, uint64_t now);
void   adsp_close(adsp_t *a, int c, uint64_t now);      // Close Advice once, then free
void   adsp_tick(adsp_t *a, uint64_t now);
int    adsp_is_open(const adsp_t *a, int c);
uint8_t           adsp_sock(const adsp_t *a, int c);    // 0 when c is no connection
const lt_route_t *adsp_peer(const adsp_t *a, int c);    // NULL when c is no connection
