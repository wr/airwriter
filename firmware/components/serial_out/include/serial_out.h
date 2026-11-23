// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer port: UART1 at 9600 8N1 into the RS-422 transceiver, with the
// printer's HSKo line wired to the ESP32's CTS so the peripheral stalls the
// FIFO on its own. SPEC.md 3.9 -- no polling loop and no software watermark.
//
// That same line is the printer's only status. serial_out reports its edges
// and nothing more; printer_state decides what a low line means (offline when
// idle, needs attention mid-job). A write waits out a low line -- a paused job
// resumes when paper returns -- and fails only after stall_abort_s with no
// progress.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    int      uart_num;
    int      tx_pin;
    int      rx_pin;
    int      cts_pin;      // the printer's HSKo. -1 disables flow control
    int      baud;

    // Line polarity insurance. The board has no jumpers: inverting the driver
    // input flips Y and Z together, which is electrically the same as swapping
    // the differential pair, and the same holds for the receiver. SPEC.md 2.4.
    int      invert_tx;
    int      invert_rx;
    int      invert_cts;   // HSKo reaches IO8 active-high instead of active-low

    // Seconds a write waits with nothing accepted before failing the job.
    // 0 uses the default of 600.
    int      stall_abort_s;

    // Reports HSKo edges (1 = ready), sampled every 100 ms, plus the level at
    // start. Called from serial_out's line task. With loopback on it reports
    // ready once (bytes do flow); with no CTS pin it reports not-ready once.
    void   (*on_line)(void *ctx, int ready);
    void    *on_line_ctx;

    // W-1102: a clock on cts_pin came or went, with its rate. Clocked is at least 100 kHz in
    // two 50 ms windows running, gone is 20 below it (serial_clock.h), so a printer switched
    // off is seen in about 2 s and a pause shorter than that is not. Only a QuickDraw
    // LaserWriter drives HSKi like that. From the line task,
    // with on_line_ctx. While clocked, on_line is not called (a level read off a clock is
    // noise); when the clock goes, on_line reports the level afresh. May be NULL.
    void   (*on_clock)(void *ctx, int clocked, int hz);

    // Loops TX back into RX inside the peripheral, so the byte stream can be
    // verified on a devkit with no transceiver and no printer attached.
    int      loopback;

    // W-1179: the socket starts on LocalTalk: the port starts parked (serial_out_park), its
    // pins never routed until it is unparked.
    int      start_parked;
} serial_out_cfg_t;

// W-1179: rev 2026.11 has a printer line on each socket (J2's on UART1, J4's on UART0). One
// job at a time, so every call below except the _of ones and serial_out_park works on the
// selected port, which callers change only under the job lock. Port 0 is J2, rev A's only
// one. Loopback is port 0's alone.
#define SERIAL_OUT_PORTS 2

// serial_out_init is serial_out_init_port(0, cfg).
esp_err_t serial_out_init(const serial_out_cfg_t *cfg);
esp_err_t serial_out_init_port(int port, const serial_out_cfg_t *cfg);
int       serial_out_present(int port);
// Which port the calls below use: 0 until changed. A port not set up is not taken.
void      serial_out_select(int port);
int       serial_out_selected(void);
// W-1179: the socket's other job, LocalTalk. Parked, the port's TX pad is an input with no
// pull, its line is not reported (on_line, on_clock) and its clock state is forgotten;
// unparked, its pins are routed back, its flow control is the link's and RX is dropped, and
// the line is reported afresh. Callers hold the job lock and never park the selected port
// mid-job.
// W-1186: a socket has one UART for whichever job it does, so a parked port has no driver on
// its UART: parking deletes it, and the socket's LocalTalk node installs its own there
// (localtalk_cfg_t.lent). Unparking installs the printer line's again, at the link's speed
// and with the line's inversion, so LocalTalk's must be gone first (localtalk_park). A port
// that starts parked starts with none. While the selected port is parked every call below
// that would touch its UART does nothing (a write fails).
void      serial_out_park(int port, int park);
int       serial_out_parked(int port);
// Another port's handshake line and clock, as serial_out_clear and serial_out_clock read the
// selected one's. Any task.
int       serial_out_clear_of(int port);
int       serial_out_clock_of(int port, int *hz);
// W-1179: framing errors, breaks and parity errors on a port's RX since the last call (none
// while it is parked): LocalTalk reaching a UART set for a printer. Empties the port's event queue, so it
// takes a QuickDraw LaserWriter's BREAK too (serial_out_break_take): only for a port with no
// clock, outside a question or a job (callers hold the job lock).
int       serial_out_junk_of(int port);

// Blocks until every byte is queued. With CTS flow control that means it
// waits while the printer is busy or paused, which is the backpressure the
// raster pipeline wants -- but not past stall_abort_s with no progress, when
// it returns ESP_FAIL with the rest of the payload abandoned. The caller owns
// failing the job.
esp_err_t serial_out_write(const uint8_t *buf, size_t n);

// Waits for the last byte to leave the shift register. At 9600 baud the FIFO
// can hold a second of data, so a job is not finished when the write returns.
// ESP_ERR_TIMEOUT if the printer stays paused past timeout_ms, ESP_FAIL at
// once if serial_out_set_abort is set; either way the tail is still queued
// for serial_out_purge.
esp_err_t serial_out_flush(uint32_t timeout_ms);

// Discards whatever is still queued in the hardware TX FIFO. For the moment a
// job is failed: without this, a dead job's tail (mid-escape-sequence, up to
// one FIFO) sits queued until paper returns and then prints ahead of the next
// job, leaving the printer in whatever mode the truncation happened in. Call
// only from the writing task with no write in flight.
void serial_out_purge(void);

// Cancel: an in-flight or later write gives up at once instead of waiting out
// a paused printer, so Cancel-Job on a paper-out printer frees it. Set from
// the cancelling task; clear it when the next job starts.
void serial_out_set_abort(int on);

// Sends bytes with CTS flow control briefly off, for unsticking a printer
// that holds HSKo false while idle (W-1039: CTRL-X clears an unprinted line
// that serial-line noise left in its buffer). The ImageWriter keeps ~27
// characters of slack after dropping DTR. Never while a job is writing --
// callers hold the job lock. ESP_ERR_INVALID_STATE with no CTS line, in
// loopback, or on a StyleWriter's link (below). Flow control goes back to what
// the link says, not to on.
esp_err_t serial_out_send_unflowed(const uint8_t *buf, size_t n);

// Asking the printer something (W-1064: ESC ?, self-ID). The printer answers
// on its TxD, which reaches the UART's RX; nothing else reads it.
// serial_out_rx_drop discards whatever RX holds, so a reply is not mistaken
// for line noise or a late answer to an earlier question. serial_out_query
// queues `cmd` behind anything already in the FIFO (flow controlled, so a
// paused printer gets it when it is ready), waits up to wait_ms for a first
// byte, then reads until 50 ms pass with nothing new or `cap` is full.
// Returns the bytes read, 0 for no answer, -1 with no CTS line or in
// loopback or on a StyleWriter's link (below). Callers hold the job lock.
void serial_out_rx_drop(void);
int  serial_out_query(const uint8_t *cmd, size_t n, uint8_t *reply, size_t cap,
                      uint32_t wait_ms);
// The cable tester (W-1055) borrows TX and RX for RMT. serial_out_lend(1)
// waits for the transmitter to drain and leaves both pins to the caller;
// serial_out_lend(0) routes them back to the UART, puts the link's flow
// control back and drops whatever RX picked up meanwhile. Callers hold the job
// lock.
void serial_out_lend(int lend);

// The printer's HSKo line right now. A printer that is attached and online
// holds this asserted; nothing plugged in reads the same as offline, which is
// the honest answer -- the line cannot tell those apart.
int serial_out_clear(void);

// Bytes read back while loopback is enabled, and their CRC-32. Zero otherwise.
void serial_out_loopback_stats(size_t *bytes, uint32_t *crc32);
void serial_out_loopback_reset(void);

// Waits for the reader to catch up with `expect` bytes. uart_wait_tx_done
// returns when the last byte has left the transmitter, which is a moment
// before the receiving task has been scheduled to collect it -- without this
// the tail of every stream looks lost. Returns 1 if the count was reached.
int serial_out_loopback_wait(size_t expect, uint32_t timeout_ms);

// The port's link (W-1103). An ImageWriter is 9600 baud with CTS, as serial_out_init
// set it up; a StyleWriter is 57600 with no flow control, because it may never
// raise HSKo. serial_out_set_link waits for the transmitter to empty (a byte held
// by a low HSKo is thrown away after 300 ms, so it cannot go out later at the
// wrong baud), sets the baud and the flow control, and drops whatever RX holds.
// Asked for the link it already has, it reconfigures nothing and only drops RX.
// `cts` is ignored on a board with no CTS line. Back to 9600 with CTS whenever
// the port's record is not a StyleWriter. A no-op in loopback. Callers hold the
// job lock.
void serial_out_set_link(int baud, int cts);

// Raw reading, for the link's own replies: waits up to first_ms for a first byte,
// then reads until gap_ms pass with nothing new (0: what is already there) or
// `cap` is full. Returns the bytes read, 0 for none, -1 in loopback. Rounded up
// to whole ticks. Callers hold the job lock.
int serial_out_read(uint8_t *b, size_t cap, uint32_t first_ms, uint32_t gap_ms);

// Raw writing for a StyleWriter's link: fills the FIFO and sleeps one tick between
// fills, so 57600 baud (5.76 KB/s) keeps up where serial_out_write's 100 ms sleeps
// (about 1.3 KB/s) would not. 0 once every byte is queued in the FIFO (serial_out_flush
// waits for the last to leave), -1 if timeout_ms passes first (what was queued stays;
// serial_out_purge drops it), or in loopback. It never waits on HSKo (a StyleWriter's
// link has no flow control) and does not look at serial_out_set_abort: a caller that
// wants to be cancellable writes in chunks and checks its own flag between them. On a
// link that has CTS a low HSKo holds the FIFO and the call times out. Callers hold the
// job lock.
int serial_out_write_raw(const uint8_t *b, size_t n, uint32_t timeout_ms);

// The clock on cts_pin (W-1102), counted by PCNT through the GPIO matrix, so the pad stays
// UART1's CTS. 1 while clocked, else 0; *hz is the last window's rate (may be NULL). It holds a
// pause of up to 2 s as clocked (serial_clock.h): the printer is there, not that bits move.
int serial_out_clock(int *hz);
// 1 if the clock on cts_pin is toggling now, read off the pad for at most about 100 us. Not for
// an ISR.
int serial_out_clock_live(void);
// The fraction of time cts_pin reads high, per mille, from 200,000 samples taken at jittered
// moments (about 0.1 s). For the log, when a clock is found. -1 with no CTS line.
int serial_out_clock_duty(void);
// 1 if the UART saw a BREAK on RX since the last call, else 0. Reads and empties the UART's
// event queue (8 deep: a BREAK behind eight other events is lost). A QuickDraw LaserWriter
// sends one to say "read my status".
int serial_out_break_take(void);
