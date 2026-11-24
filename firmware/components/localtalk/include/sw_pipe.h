// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pipe between a Mac's StyleWriter driver and the printer's port (W-1193,
// StyleWriters design, "Phase 2", "On the board"): two rings, the job's end,
// and one round of the pump that moves the bytes. Pure C with no state of its
// own, like pap_relay: the owner allocates a sw_pipe_t (PSRAM on the board)
// and the host tests in tools/rip drive it.
//
// Two tasks share it. The link's task, with its link's lock held, is the
// adapter's job_* calls: room, put, output, drained, end. The pump owns the
// port: sw_pipe_pump takes what the Mac sent, writes it, and puts what the
// printer said into the other ring. The lock hooks guard what is between
// them and are never held across a write or a read. The pump is told how the
// job ended and never calls the adapter: SW_PUMP_STOP asks its owner to (on
// the board, swa_stop under that link's lock), so each call runs on its own
// task.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define SW_PIPE_DOWN  4096      // toward the printer: the ADSP window
#define SW_PIPE_UP    512       // the printer's bytes waiting for the link

struct sw_pipe {
    uint8_t  down[SW_PIPE_DOWN], up[SW_PIPE_UP];
    size_t   d_head, d_len, u_head, u_len;
    uint32_t down_total, up_total;  // put by the link, heard from the printer
    uint32_t taken, wrote;          // of down: handed to the pump, and written by it
    uint32_t reached;               // of taken: a write was tried, so the printer may have them
    uint32_t up_lost;               // the printer's bytes that found the up ring full
    uint8_t  ended, clean;          // the adapter's job_end, and its verdict
    uint8_t  stop, failed, told;    // the board's stop; the port failed; the owner was asked to end it
    uint8_t *cap;                   // the first cap_max bytes toward the printer, kept
    size_t   cap_max, cap_len;
    void   (*lock)(void *ctx);
    void   (*unlock)(void *ctx);
    void    *lock_ctx;
};
typedef struct sw_pipe sw_pipe_t;

// Once: zeroes it and keeps the lock hooks (NULL: one task, as the tests).
void   sw_pipe_init(sw_pipe_t *p, void (*lock)(void *ctx), void (*unlock)(void *ctx), void *ctx);
// A job begins: empty, counters zero, not ended, no capture.
void   sw_pipe_begin(sw_pipe_t *p);
// Where the job's first `max` bytes toward the printer are kept, as the pump
// takes them. After sw_pipe_begin; NULL for none.
void   sw_pipe_capture(sw_pipe_t *p, uint8_t *buf, size_t max);
const uint8_t *sw_pipe_captured(const sw_pipe_t *p, size_t *len);

// ---- The link's side (the adapter's job_* ops)
size_t sw_pipe_room(sw_pipe_t *p);
void   sw_pipe_put(sw_pipe_t *p, const uint8_t *d, size_t n);      // no more than room
size_t sw_pipe_output(sw_pipe_t *p, uint8_t *buf, size_t max);     // the printer's bytes
// Everything put has been written to the port: the ring is empty and the
// pump has finished writing what it took.
int    sw_pipe_drained(sw_pipe_t *p);
// The adapter's job_end. Nothing put after it is kept.
void   sw_pipe_end(sw_pipe_t *p, int clean);

// ---- Any task
void   sw_pipe_stop(sw_pipe_t *p);          // the board ends the job (the test button)
void   sw_pipe_fail(sw_pipe_t *p);          // the port cannot be had: nothing more goes to it

// ---- The pump's side
size_t sw_pipe_take(sw_pipe_t *p, uint8_t *buf, size_t max);       // bytes for the port
void   sw_pipe_wrote(sw_pipe_t *p, size_t n);                      // it finished writing n of them
void   sw_pipe_from_printer(sw_pipe_t *p, const uint8_t *d, size_t n);

// The port, as the pump uses it.
typedef struct {
    // 0, or nonzero when the port would not take them: the job fails.
    int  (*write)(void *ctx, const uint8_t *d, size_t n);
    // Waits for the last byte written to leave the port: 0, or nonzero as
    // write. Called when the ring has emptied, before those bytes count as
    // written. NULL: a write is done when it returns.
    int  (*flush)(void *ctx);
    // What the printer has sent, up to cap: the count, 0 for none. `idle`:
    // nothing went to the port this round, so it waits a moment (a tick on
    // the board) for a first byte, also when cap is 0, and the pump's loop
    // does not spin.
    int  (*read)(void *ctx, uint8_t *buf, size_t cap, int idle);
    void *ctx;
} sw_pump_io_t;

typedef enum {
    SW_PUMP_GO,         // call again
    // The board's stop, or the port failed, with the job still the adapter's:
    // have the adapter end it (swa_stop), then call again. Said once.
    SW_PUMP_STOP,
    SW_PUMP_CLEAN,      // ended cleanly and every byte is written: nothing to undo
    // Ended any other way. What was not written is dropped; the printer is
    // owed a reset if sw_pipe_reached.
    SW_PUMP_UNCLEAN,
} sw_pump_t;

// One round: up to cap bytes from the Mac to the port, and what the printer
// has to say into the up ring, never more than it has room for. buf is the
// pump's scratch, cap bytes.
sw_pump_t sw_pipe_pump(sw_pipe_t *p, const sw_pump_io_t *io, uint8_t *buf, size_t cap);

uint32_t sw_pipe_down_total(const sw_pipe_t *p);
uint32_t sw_pipe_up_total(const sw_pipe_t *p);
uint32_t sw_pipe_reached(const sw_pipe_t *p);   // bytes a write was tried for
int      sw_pipe_stopped(const sw_pipe_t *p);
int      sw_pipe_failed(const sw_pipe_t *p);
