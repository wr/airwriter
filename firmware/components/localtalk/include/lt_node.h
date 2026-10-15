// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LocalTalk node, pure C: frames and the clock in, frames and log lines
// out. No FreeRTOS, no UART -- localtalk.c feeds it what TashTalk relays and
// sends what it asks for; the host tests in tools/rip drive it with a fake
// clock. Every timer is evaluated in lt_node_tick() against the caller's
// `now` (milliseconds, uint64 so it never wraps).
//
// What it does, in order:
//  1. LLAP: acquires a server node ID (128-254) by sending lapENQs to a
//     candidate and moving on whenever anything answers or uses it, then hands
//     the ID to TashTalk (ops.claim), which answers ENQ and RTS from then on.
//  2. NBP: answers LkUp for object:type@* -- the queue name and the type the
//     Chooser's driver looks up -- with the server listening socket (SLS).
//  3. PAP over ATP, as Apple's ImageWriter II/LQ LocalTalk Option Card does
//     it: answers SendStatus with the card's two-byte status word, accepts
//     one connection at a time (OpenConn), reads the job with SendData, keeps
//     the connection tickled, and answers CloseConn. The job's bytes -- raw
//     ImageWriter commands -- go to ops.pap_data.
// Also: AEP echo (socket 4), and the network number from RTMP when a router
// speaks.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"

#define LT_SOCK_SLS   0x81      // PAP server listening socket, registered with NBP
#define LT_SOCK_CONN  0x82      // the one connection's ATP responding socket
#define LT_FLOW       8         // our flow quantum: 512-byte packets per SendData

// LLAP node acquisition. Appendix B has servers send 120 back-to-back ENQs,
// 200 us apart, and TailTalk sends 8; tashrouter spaces its 8 by 250 ms. A
// UART round trip sits between us and any ACK, so these are spread over a
// second, which also gives a node busy for a moment the chance to answer.
#define LT_ENQ_TRIES  20
#define LT_ENQ_GAP_MS 50

// The Option Card's status word (Apple TN NW20; bits confirmed on a real
// ImageWriter II by TailTalk). It goes on the wire as a two-byte Pascal
// string, low byte first. Ready with a black ribbon is 0x0000; a deselected
// printer reads 0x0008, out of paper 0x002A. LT_IW_BUSY is for the board
// printing someone else's job: a Mac told "busy" on its own open connection
// sits on it and never sends (bench, 2026-10-15). TailTalk never sets it.
#define LT_IW_BUSY      0x8000
#define LT_IW_RIBBON    0x0080   // colour ribbon fitted
#define LT_IW_FEEDER    0x0040
#define LT_IW_PAPER_OUT 0x0020
#define LT_IW_COVER     0x0010
#define LT_IW_OFFLINE   0x0008
#define LT_IW_JAM       0x0004
#define LT_IW_FAULT     0x0002
#define LT_IW_ACTIVE    0x0001

// PAP timers (Inside AppleTalk ch. 10).
#define LT_PAP_CONN_TIMEOUT_MS 120000   // nothing from the other end: tear down
#define LT_PAP_TICKLE_MS        60000   // half the connection timer
#define LT_PAP_SENDDATA_RETRY_MS 15000
// A workstation's own read of us (its SendData) is answered empty once it has
// sent nothing for this long, so the read does not time out; TailTalk's PAP
// server, tested on hardware, uses 2 s. Apple's AppleTalk ImageWriter 7.0.1
// never reads (bench, 2026-10-15); a LaserWriter-style client does.
#define LT_PAP_READ_IDLE_MS      2000
// An OpenConn from the Mac that holds the open connection, which has sent no
// data for this long, means it gave that one up (the Mac kept tickling a dead
// connection and retried OpenConn 18 times against "busy" on the bench). The
// old connection is closed and the new one accepted. Another Mac still gets busy.
#define LT_PAP_STALE_MS         10000

typedef struct {
    void *ctx;
    // One LLAP frame (dst, src, type, data), without its FCS.
    void (*send)(void *ctx, const uint8_t *frame, size_t n);
    // The node ID is ours: tell TashTalk to answer ENQ/RTS for it (and
    // remember it for next boot). 0 = answer for nothing (re-acquiring).
    void (*claim)(void *ctx, uint8_t node);
    // A line for the log and the web view.
    void (*log)(void *ctx, const char *line);
    // Job bytes from the open PAP connection, in order; eof on the last call.
    // May be NULL.
    void (*pap_data)(void *ctx, const uint8_t *d, size_t n, int eof);
} lt_ops_t;

typedef struct {
    const char *object;     // NBP object: the queue name (UTF-8; non-ASCII becomes '?')
    const char *type;       // NBP type the Chooser looks up
    uint16_t    status;     // the Option Card status word to start with
    uint8_t     hint;       // the node ID to try first (last boot's), 0 = random
    uint32_t    seed;       // nonzero random seed for candidate IDs
} lt_cfg_t;

typedef enum { LT_ACQUIRING = 0, LT_ONLINE } lt_phase_t;

typedef struct {               // how to answer whoever sent a datagram
    lt_addr_t addr;            // their socket
    uint8_t   via;             // LLAP destination: them, or the router that relayed it
    uint8_t   is_long;
} lt_route_t;

#define LT_XO_SLOTS 4
#define LT_XO_PKT   (ATP_HDR + 4 + 256)   // the biggest response we cache: OpenConnReply

typedef struct {               // a sent XO response, kept for duplicate requests
    uint8_t    used, src_sock;
    lt_route_t to;
    uint16_t   tid;
    uint64_t   expires;
    uint16_t   len;
    uint8_t    atp[LT_XO_PKT];
} lt_xo_t;

typedef struct {
    uint8_t    open, connid;
    lt_route_t ws;             // the workstation's ATP responding socket
    uint64_t   heard, tickled;
    uint16_t   tickle_tid;
    // Our read (SendData to the workstation).
    uint16_t   seq, rd_tid;
    uint8_t    rd_want;        // response packets still wanted; 0 = no read out
    uint8_t    rd_eof, done;   // done: EOF delivered, nothing more to read
    uint64_t   rd_sent;
    uint16_t   rd_len[LT_FLOW];
    uint8_t    rd_buf[LT_FLOW][PAP_QUANTUM];
    // The workstation's read of us (its SendData), answered empty once it
    // has been quiet LT_PAP_READ_IDLE_MS, or with EOF once our side is done.
    uint8_t    ws_read, eof_sent;
    uint16_t   ws_read_tid, ws_read_seq;
    lt_route_t ws_read_from;
    uint16_t   ws_answered_seq;  // the last one answered, for PAP-level retransmits
    uint8_t    ws_answered_eof;
    uint64_t   wrote;            // when the workstation last sent us Data
    uint32_t   bytes;
} lt_pap_t;

typedef struct {
    uint32_t frames, for_us, lookups, replies, opens, busy, statuses, jobs, dup_suspect, echoes;
    uint32_t ws_reads, ws_answers;   // the Mac's reads of us, and our answers
} lt_stats_t;

typedef struct {
    lt_ops_t   ops;
    uint8_t    obj[NBP_NAME_MAX + 1], type[NBP_NAME_MAX + 1];
    uint16_t   status;
    lt_phase_t phase;
    uint8_t    node;           // ours when ONLINE; the candidate while ACQUIRING
    uint8_t    clash;
    uint16_t   enq_sent;
    uint32_t   candidates;     // IDs tried, including the current one
    uint64_t   enq_at;
    uint16_t   net;            // from RTMP; 0 until a router speaks
    uint32_t   rng;
    uint16_t   tid;            // next ATP TID for our requests
    lt_xo_t    xo[LT_XO_SLOTS];
    lt_pap_t   pap;
    lt_stats_t st;
    // The last LkUp logged, so a Chooser asking every second logs once.
    uint8_t    last_lkup[3 * (NBP_NAME_MAX + 1) + 2];
    uint64_t   last_lkup_at;
    uint8_t    heard[32];      // bit n: node n has sent a frame
    uint64_t   echo_at;        // when lt_node_echo last asked
} lt_node_t;

void lt_node_init(lt_node_t *n, const lt_cfg_t *cfg, const lt_ops_t *ops, uint64_t now);
// One frame as TashTalk relayed it, FCS already checked and stripped.
void lt_node_rx(lt_node_t *n, const uint8_t *f, size_t len, uint64_t now);
// Timers: enquiries while acquiring, PAP retries, tickles and timeouts.
void lt_node_tick(lt_node_t *n, uint64_t now);
// Send an AEP echo request to `node` (socket 4); its reply is logged. A bench
// check that frames get out and back. Does nothing until the node is online.
void lt_node_echo(lt_node_t *n, uint8_t node, uint64_t now);
// The Option Card status word, e.g. when the printer goes offline.
void lt_node_set_status(lt_node_t *n, uint16_t status);
