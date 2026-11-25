// SPDX-License-Identifier: GPL-3.0-or-later
//
// A Mac's own StyleWriter driver printing through the board (W-1193,
// StyleWriters design, "Phase 2", "On the board"), as every AppleTalk link
// shares it: each link has an adapter (sw_adapter.h) riding its node, and all
// of them print through the one job here, one at a time on the whole board.
//
// The link's task takes the printer without waiting (main.c's claim: the job
// lock and the checks on the port), so a Mac hears busy at once, and a task
// made for the job, the pump, owns the port from there: it moves bytes
// between the pipe's two rings (sw_pipe.h) and the port through main.c's
// hooks, and at the end hands main.c the verdict, which resets the printer if
// one is due and gives the job lock. A link's task never waits on the port.
// The job's first 256 KB toward the printer are kept for GET /api/sw/job, in
// at_printer.h's capture.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "at_printer.h"
#include "sw_adapter.h"

// How a job ended, for main.c's close.
typedef struct {
    int      clean;         // the Mac ended it and every byte reached the port
    int      stopped;       // the board stopped it (the test button)
    int      failed;        // the port could not be had, or would not take the bytes
    uint32_t down, up;      // bytes from the Mac for the printer, and the printer's back
    uint32_t reached;       // of down, those a write to the port was tried for
} at_swa_end_t;

// The port, as main.c owns it. claim, unclaim and begin run on a link's task
// with that link's lock held and must not wait; open to close on the pump.
typedef struct {
    void *ctx;
    int  (*busy)(void *ctx);                                 // the board prints something, whose ever
    // Take the printer for record `id`, for a Mac on `link`: 0 with the job
    // lock held, nonzero for busy (the lock is someone's, or the record's
    // printer is not the one on its port now) with nothing held.
    int  (*claim)(void *ctx, at_link_t link, int id);
    void (*unclaim)(void *ctx);                              // the pump could not be made: let go
    void (*begin)(void *ctx);                                // the job's books; the pump runs after
    int  (*open)(void *ctx);                                 // the port, at the StyleWriter's link: 0, or nonzero
    int  (*write)(void *ctx, const uint8_t *d, size_t n);    // sw_pump_io_t's three
    int  (*flush)(void *ctx);
    int  (*read)(void *ctx, uint8_t *buf, size_t cap, int idle);
    // The job is over: what is left is dropped or has gone, the printer is
    // reset if the verdict calls for it, the port goes back to its record's
    // link and the job lock is given.
    void (*close)(void *ctx, const at_swa_end_t *e);
    // A Mac renamed record `id` (UTF-8). On a link's task, its lock held: it
    // must only note it for a task that may write flash.
    void (*rename)(void *ctx, int id, const char *name);
} at_swa_port_t;

// Once, before any link starts; later calls keep the first.
void at_swa_init(const at_swa_port_t *port);

// A link's side of its adapter: where its packets and log lines go, and how
// the pump has it end the job (swa_stop under that link's lock).
typedef struct {
    at_link_t link;
    void     *ctx;
    void    (*send)(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n);
    void    (*log)(void *ctx, const char *line);
    void    (*stop)(void *ctx);
} at_swa_owner_t;

// An adapter for a link, in PSRAM, its ops bound to the job here; `o` must
// outlive it. NULL with no memory. The link attaches it to its node
// (lt_node_attach_swa) and offers it its names (swa_offer).
sw_adapter_t *at_swa_make(const at_swa_owner_t *o, uint32_t seed);

// The test button: the job is stopped. Only sets a flag; the pump tells the
// Mac and resets the printer. 1 if a job was running. Any task.
int at_swa_stop(void);
// The last job's bytes toward the printer as captured (up to 256 KB), or NULL.
const uint8_t *at_swa_bytes(size_t *len);
