// SPDX-License-Identifier: GPL-3.0-or-later
//
// Apple's StyleWriter network adapter, stood in for (W-1193, StyleWriters
// design, "Phase 2", "The adapter"): what a vintage Mac's own StyleWriter
// driver finds in the Chooser and prints through. One per AppleTalk link,
// riding the link's node as the PAP client does. Pure C, no state of its own:
// the owner allocates a sw_adapter_t (PSRAM on the board) and the host tests
// in tools/rip drive it with a fake clock.
//
// - Up to SWA_NAMES StyleWriters, each under its own name and type with a
//   listening socket of its own, SWA_SOCK(i), which is also where its
//   call-back comes from.
// - A Mac opens an ADSP connection to that socket and asks with attention
//   messages. Every one is answered on that connection's stream, 2 bytes
//   unless noted, an unknown one with 00 00, so the Mac never hangs.
// - 000B asks to print (70 bytes: the Mac's data socket in byte 3). Accepted
//   (00 00), the adapter opens a connection back to that socket at the first
//   of: the Mac closing the one it asked on, 0012 on it, SWA_CALL_MS. That
//   second connection is the pipe: the Mac's stream goes to the printer as
//   it is and the printer's bytes go back. Busy is 00 01, refused FF FF.
// - On the pipe: 0006 (has everything reached the printer?), 000A (status
//   text, kept for 0010), 0012 (the end: the close that follows is clean).
//   The fast modes are refused (0005, 0007) and a message that is the one
//   byte 'x' or 'h', which would switch the printer to one, is not passed on.
// - From any Mac, any time: 0011 and 000D (is it busy?), 000E (whose job?),
//   0010 (its status text), 000C (withdraw a request not yet in the pipe),
//   0009 then 0012 on one connection (rename).
// - One job at a time; SWA_CONTROLS connections beside the pipe. A Mac
//   beyond that is not answered and its driver tries again.
//
// The request and the status text hold a person's name and words: they are
// kept only to answer 000E and 0010 while that job lives, zeroed when it
// ends, and never logged.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"
#include "lt_node.h"
#include "adsp.h"

// SWA_NAMES, SWA_SOCK(i) and swa_name_t are lt_node.h's: the node's offers carry them.
#define SWA_TYPE_COLOR "ColorStyleWriter2400AT"   // 2200, 2400, 2500
#define SWA_TYPE_MONO  "StyleWriter1200AT"        // original, II, 1200, 1500
#define SWA_REQ_LEN  70

#define SWA_CONTROLS    (ADSP_CONNS - 1)  // connections beside the pipe, whose place is kept
#define SWA_CTL_WINDOW  1716              // what Apple's adapter tells a Mac it takes
#define SWA_CALL_MS     1000              // from the accept to the call-back, at the latest
#define SWA_PIPE_MS     15000             // from the accept to the pipe, or the job is dropped
#define SWA_STATUS_LEN  257               // 000A: a message ID, a Pascal string, padding
#define SWA_ANSWER_ROOM 32                // of the pipe's send buffer, kept from the printer's bytes

// The attention codes a Mac's driver sends.
enum {
    SWA_SERIAL_CONFIG = 0x0003, SWA_SERIAL_OPTIONS = 0x0004, SWA_FAST_CLOCK = 0x0005,
    SWA_DRAINED = 0x0006, SWA_FAST_SERIAL = 0x0007, SWA_SERIAL_LOWER = 0x0008,
    SWA_SET_NAME = 0x0009, SWA_SET_STATUS = 0x000A, SWA_PRINT = 0x000B, SWA_WITHDRAW = 0x000C,
    SWA_QUEUE_POS = 0x000D, SWA_JOB_USER = 0x000E, SWA_JOB_STATUS = 0x0010,
    SWA_JOB_COUNT = 0x0011, SWA_END = 0x0012,
};

typedef struct {
    void  *ctx;
    void   (*send)(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n);
    void   (*log)(void *ctx, const char *line);
    int    (*busy)(void *ctx);                     // the board prints something, whose ever
    // Take the printer for record id: 0 taken, nonzero busy. Must not wait.
    int    (*job_begin)(void *ctx, uint8_t id);
    void   (*job_up)(void *ctx);                   // the pipe is open
    // Asked from job_begin's 0 on: the call-back's Open Request carries it.
    size_t (*job_room)(void *ctx);
    void   (*job_data)(void *ctx, const uint8_t *d, size_t n);   // to the printer, in order
    size_t (*job_output)(void *ctx, uint8_t *buf, size_t max);   // the printer's bytes waiting
    int    (*job_drained)(void *ctx);
    void   (*job_end)(void *ctx, int clean);       // once per job_begin that returned 0
    void   (*rename)(void *ctx, uint8_t id, const uint8_t *pname);  // Pascal, MacRoman
} swa_ops_t;

typedef struct {
    uint8_t open;                     // a connection a Mac opened to us
    uint8_t staged[NBP_NAME_MAX + 1]; // 0009's name, waiting for 0012 on this connection
} swa_conn_t;

struct sw_adapter {
    swa_ops_t  ops;
    swa_name_t names[SWA_NAMES];
    uint8_t    job;                   // 0 none, 1 waiting for the pipe, 2 piping
    uint8_t    job_name, job_id;      // which name, and its record
    int8_t     ctl;                   // the connection the request came on, -1 once it closed
    int8_t     pipe;                  // the call-back's connection, -1 with none
    uint8_t    called;                // the call-back has been made
    uint8_t    opening;               // inside adsp_open for the pipe: its place is not known yet
    uint8_t    clean;                 // 0012 came on the pipe
    uint8_t    mid;                   // the pipe is inside a message
    uint8_t    held, held_byte;       // a lone 'x' or 'h' whose message has not ended
    lt_route_t mac;                   // the Mac's data socket
    uint64_t   accepted_at, now;
    uint8_t    req[SWA_REQ_LEN + 2];          // the accepted request, then 000E's 00 00
    uint8_t    status[SWA_STATUS_LEN + 2];    // the last 000A, then 0010's 00 00
    swa_conn_t conn[ADSP_CONNS];
    adsp_t     adsp;
};
typedef struct sw_adapter sw_adapter_t;

// seed: where the ADSP connection IDs count on from.
void swa_init(sw_adapter_t *a, const swa_ops_t *ops, uint32_t seed);
void swa_reset(sw_adapter_t *a);                   // the node was re-made: a job ends unclean
// The names to answer for, n of them (the rest none). A name whose id changed
// or went: its connections close and a job on it ends as swa_stop.
void swa_offer(sw_adapter_t *a, const swa_name_t *names, int n, uint64_t now);
int  swa_lookup(const sw_adapter_t *a, const nbp_tuple_t *q, int from, swa_name_t const **hit); // next match at or after index from, or -1
void swa_rx(sw_adapter_t *a, const ddp_t *d, uint64_t now);     // a type 7 datagram to SWA_SOCK(i)
void swa_tick(sw_adapter_t *a, uint64_t now);
void swa_stop(sw_adapter_t *a, uint64_t now);      // the board ends the job: close, job_end(0)
int  swa_job(const sw_adapter_t *a);               // 0 none, 1 waiting for the pipe, 2 piping
