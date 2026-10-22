// SPDX-License-Identifier: GPL-3.0-or-later
//
// The board as a PAP client (W-1082, laser printers design, "The PAP
// client"): it opens a connection to a LaserWriter and sends a job, beside the
// PAP server the ImageWriter answers with, on the same node and its ATP and
// NBP. Pure C and link-agnostic like lt_node: frames and the clock in through
// the node it is attached to (lt_node_attach), frames out through it. The
// host tests in tools/rip drive it with a fake clock.
//
// The printer pulls. Its SendData asks for up to 8 packets; each answer is at
// most 8 Data packets of at most 512 bytes (a LaserWriter on LocalTalk takes
// no more: TailTalk, on hardware), user bytes [connID, 4, EOF, 0] on every
// packet. A repeated SendData (the same sequence number, whatever its TID)
// gets the same packets again. Meanwhile the client reads the printer's own
// output with SendData of its own, from the start: status lines, a PostScript
// error, the query job's answers. The job is done when the printer's output
// ends (its EOF) after ours, or 30 s after ours if it never says so (netatalk:
// some printers do not on big jobs); then CloseConn.
//
// Constants checked against TailTalk's pap.rs (FeralFirmware/TailTalk, read
// as a reference): 512-byte packets, quantum 8, busy retried every 2 s,
// tickles every 30 s, the connection dead after 120 s silent, 8 replies kept
// for retransmits. Its OpenConn sends a wait time of 0; this one counts the
// seconds since the first try, as the design says, so the printer can grant
// the longest waiter.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"
#include "lt_node.h"

#define PAPC_SOCK          LT_SOCK_PAPC     // OpenConn, data, CloseConn: our responding socket
#define PAPC_SOCK_ST       LT_SOCK_PAPC_ST  // SendStatus
#define PAPC_FLOW          8                // our quantum: packets per printer SendData
#define PAPC_CACHE         8                // replies kept for retransmits
#define PAPC_BUSY_RETRY_MS 2000
#define PAPC_TICKLE_MS     30000
#define PAPC_DEAD_MS       120000
#define PAPC_GONE_MS       30000            // no answer at all to OpenConn
#define PAPC_RETRY_MS      2000             // OpenConn, CloseConn, SendStatus retransmits
#define PAPC_TRIES         3                // CloseConn and SendStatus, at most
#define PAPC_READ_RETRY_MS 15000            // our SendData to the printer
#define PAPC_EOF_WAIT_MS   30000            // after our EOF, for the printer's
#define PAPC_HOLD_MS       500              // a SendData held for a fuller answer, at most

typedef enum {
    PAPC_END_OK,          // sent, EOF given, the printer's EOF (or the wait) after it
    PAPC_END_CANCELED,    // papc_cancel
    PAPC_END_BUSY,        // still busy when the busy wait ran out
    PAPC_END_DEAD,        // no answer to OpenConn for PAPC_GONE_MS, or silent too long once open
    PAPC_END_REFUSED,     // OpenConn answered with a result that is not busy
    PAPC_END_DROPPED,     // the link went away, or the printer closed before our EOF
} papc_end_t;

typedef struct {
    void   *ctx;
    // The job: up to max bytes into buf without waiting; *eof once nothing
    // more will come. Returns the count.
    size_t (*read)(void *ctx, uint8_t *buf, size_t max, int *eof);
    // The printer's output, in order.
    void   (*output)(void *ctx, const uint8_t *d, size_t n);
    // A status string (Pascal): SendStatus's answer, or a busy OpenConn's.
    void   (*status)(void *ctx, const uint8_t *pstr);
    void   (*opened)(void *ctx);
    // The connection is over; once per papc_open.
    void   (*ended)(void *ctx, papc_end_t how);
    void   (*log)(void *ctx, const char *line);
} papc_ops_t;

enum { PAPC_IDLE = 0, PAPC_OPENING, PAPC_OPEN, PAPC_DRAINING, PAPC_CLOSING };

typedef struct {                         // a printer SendData not yet answered
    uint8_t    used, want;
    uint16_t   seq, tid;
    lt_route_t from;
    uint64_t   since;
} papc_pend_t;

typedef struct {                         // a SendData answered, kept for retransmits
    uint8_t  used, n, eof;
    uint16_t seq;
    uint16_t len[PAPC_FLOW];
    uint8_t  pkt[PAPC_FLOW][PAP_QUANTUM];
} papc_reply_t;

typedef struct pap_client {
    papc_ops_t   ops;
    lt_node_t   *node;                   // set by lt_node_attach
    uint8_t      phase, connid, quantum, cancel;
    lt_route_t   sls, sess;              // the printer's listening and session sockets
    uint32_t     busy_ms, dead_ms;
    uint64_t     first_try, answered, heard, tickled;
    // OpenConn
    uint16_t     open_tid;
    uint8_t      open_out;               // outstanding: retransmitted until answered
    uint64_t     open_at, next_open;
    // The printer's reads of us
    papc_pend_t  pend[PAPC_CACHE];
    papc_reply_t cache[PAPC_CACHE];
    uint8_t      cache_next;
    uint8_t      stage[PAPC_FLOW * PAP_QUANTUM];
    size_t       staged;
    uint8_t      src_eof, eof_sent, eof_got;
    uint64_t     eof_at;
    // Our reads of the printer
    uint16_t     rd_seq, rd_tid;
    uint8_t      rd_want, rd_eof;
    uint64_t     rd_sent;
    uint16_t     rd_len[PAPC_FLOW];
    uint8_t      rd_buf[PAPC_FLOW][PAP_QUANTUM];
    // SendStatus
    lt_route_t   st_to;
    uint16_t     st_tid;
    uint8_t      st_tries;
    uint64_t     st_at;
    // CloseConn
    uint16_t     close_tid;
    uint8_t      close_tries;
    uint64_t     close_at;
    papc_end_t   end;
    uint32_t     sent_bytes, out_bytes;
    uint32_t     rng;
} pap_client_t;

void papc_init(pap_client_t *c, const papc_ops_t *ops, uint32_t seed);
// Opens a connection to the printer's listening socket and sends the job
// (ops.read) to its EOF. Busy is retried every 2 s for up to busy_ms. 0, or
// -1 when a connection is already under way or the client is not attached.
int  papc_open(pap_client_t *c, const lt_route_t *to, uint32_t busy_ms, uint64_t now);
// EOF goes with the next Data answer: the printer meets it inside an
// imagemask, raises an error and flushes. Before the connection is open, it
// ends at once.
void papc_cancel(pap_client_t *c, uint64_t now);
// One SendStatus to a printer's listening socket; the answer reaches ops.status.
void papc_status(pap_client_t *c, const lt_route_t *to, uint64_t now);
int  papc_idle(const pap_client_t *c);
// The link went away: the connection ends DROPPED.
void papc_link_down(pap_client_t *c, uint64_t now);
void papc_set_dead_ms(pap_client_t *c, uint32_t ms);      // tests; init sets PAPC_DEAD_MS
// From lt_node only: ATP to PAPC_SOCK or PAPC_SOCK_ST, and the clock.
void papc_rx(pap_client_t *c, const ddp_t *d, const atp_t *a, const lt_route_t *from,
             uint64_t now);
void papc_tick(pap_client_t *c, uint64_t now);
