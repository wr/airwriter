// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PAP relay (W-1088, printers design, "The relay"): a Mac's connection to
// a printer AirWriter offers on one AppleTalk link, carried to the printer on
// another by the board's PAP client. Everything passes through both ways and
// nothing is rewritten: the Mac's data to the printer, the printer's output
// (PostScript query answers, errors) back to the Mac's reads, and SendStatus
// answered with the printer's own status. The Mac hears busy until the
// printer's connection is open, and while the printer is busy.
//
// W-1095: a connection carries any number of jobs, as a Mac's LaserWriter
// driver sends them (a query, its answer read to the printer's EOF, then the
// document). Each EOF passes through as a job's end, both ways, and the
// printer's connection stays open as long as the Mac's: closed normally when
// the Mac closes between jobs, cancelled when it closes or goes mid-job.
//
// Pure, like lt_node and pap_client. Each side calls in from its own link's
// task: the Mac's node (the registration's job_* and reg_* ops) and the
// client (its papc ops). The lock hooks guard the buffers between them. The
// relay never calls either side: pap_relay_tick says what the board does next
// (open the client, cancel it, end the Mac's connection), so each call runs
// on its own link's task under its own lock.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_node.h"
#include "pap_client.h"

#define PR_RELAY_WAIT_MS  20000      // an open printer waits this long for the Mac to come back

enum { PR_RELAY_IDLE, PR_RELAY_ASKING, PR_RELAY_OPEN, PR_RELAY_BOTH, PR_RELAY_ENDING };
// What pap_relay_tick asks of the board, as bits.
enum { PR_RELAY_DO_OPEN = 1,         // open the client to the printer (papc_open_multi), once
       PR_RELAY_DO_CANCEL = 2,       // end the client's connection (papc_cancel), once
       PR_RELAY_DO_ABORT_MAC = 4,    // end the Mac's (lt_node_pap_abort on its node), once
       PR_RELAY_DO_DONE = 8,         // both ends are over: the job lock and the client are free
       PR_RELAY_DO_CLOSE = 16 };     // W-1095: close the client's normally (papc_close), once

typedef struct { uint8_t *buf; size_t cap, head, tail; } pr_ring_t;

typedef struct pap_relay {
    uint8_t          state;
    const lt_node_t *mac;            // the Mac's node and the registration it opened
    uint8_t          reg;
    uint64_t         since;
    pr_ring_t        m2p, p2m;       // the Mac's data, the printer's output
    uint8_t          asked, mac_eof, mac_gone, mac_had, prn_open, prn_ended, cancelled, aborted;
    // W-1095, per job: the printer's EOF not yet given to the Mac, the Mac's
    // bytes since its last EOF; and the connection's own end given to it.
    uint8_t          prn_eof, mac_mid, told, closed;
    int8_t           prn_end;        // papc_end_t once ended
    uint8_t          last[LT_RELAY_PKT];
    uint16_t         last_n;
    uint8_t          last_eof;
    uint8_t          status[256];    // the printer's last status, Pascal
    uint32_t         to_printer, to_mac;
    uint32_t         jobs, answered; // the Mac's EOFs, and the printer's
    void           (*lock)(void *ctx);
    void           (*unlock)(void *ctx);
    void            *lock_ctx;
} pap_relay_t;

// Buffers of cap bytes each way (16 KB on the board, in PSRAM).
void   pap_relay_init(pap_relay_t *r, uint8_t *m2p, uint8_t *p2m, size_t cap);
// A Mac's OpenConn on registration reg of node mac: 0 to accept (the printer's
// connection is open for it), 1 for busy (asked of the printer now, the
// printer busy, or another Mac's job in the relay).
int    pap_relay_mac_open(pap_relay_t *r, const lt_node_t *mac, int reg, uint64_t now);
int    pap_relay_active(const pap_relay_t *r);
int    pap_relay_tick(pap_relay_t *r, uint64_t now);     // PR_RELAY_DO_* bits

// The Mac's side, from its node's ops while the connection is the relay's.
void   pap_relay_mac_data(pap_relay_t *r, const uint8_t *d, size_t n);
size_t pap_relay_mac_room(pap_relay_t *r);
// The Mac's connection is over: normal when it closed it, 0 when abandoned.
void   pap_relay_mac_end(pap_relay_t *r, int normal);
// The Mac's EOF (W-1095): this job is all in. Its next job's data waits
// (pap_relay_mac_room is 0) until this EOF has gone to the printer.
void   pap_relay_mac_eof(pap_relay_t *r);
size_t pap_relay_mac_output(pap_relay_t *r, uint8_t *buf, size_t max, int *eof, int again);
size_t pap_relay_status(pap_relay_t *r, uint8_t *pstr, size_t cap);

// The printer's side, from the client's ops while it serves the relay.
size_t pap_relay_prn_read(pap_relay_t *r, uint8_t *buf, size_t max, int *eof);
void   pap_relay_prn_output(pap_relay_t *r, const uint8_t *d, size_t n);
void   pap_relay_prn_status(pap_relay_t *r, const uint8_t *pstr);
void   pap_relay_prn_opened(pap_relay_t *r);
// The printer's EOF for this job (the client's job_eof, W-1095).
void   pap_relay_prn_eof(pap_relay_t *r);
void   pap_relay_prn_ended(pap_relay_t *r, papc_end_t how);
