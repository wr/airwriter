// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer port: UART1 at 9600 8N1 into the RS-422 transceiver, with the
// printer's HSKo line wired to the ESP32's CTS so the peripheral stalls the
// FIFO on its own. SPEC.md 3.9 -- no polling loop and no software watermark.
//
// That same line is the paper sensor. An ImageWriter II deasserts HSKo while
// it is busy, which is normal and brief; if it stays deasserted past the
// watchdog timeout the printer is out of paper, jammed, or switched offline.
// That is reported up so the phone shows a real alert.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    SERIAL_OUT_READY,      // clear to send, or idle
    SERIAL_OUT_BLOCKED,    // HSKo deasserted longer than the watchdog allows
} serial_out_state_t;

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

    // Seconds of continuous HSKo deassertion before the printer is called
    // blocked. 0 uses the default of 30.
    int      stall_timeout_s;

    // Reports a state change. Called from the watchdog task.
    void   (*on_state)(void *ctx, serial_out_state_t st);
    void    *on_state_ctx;

    // Loops TX back into RX inside the peripheral, so the byte stream can be
    // verified on a devkit with no transceiver and no printer attached.
    int      loopback;
} serial_out_cfg_t;

esp_err_t serial_out_init(const serial_out_cfg_t *cfg);

// Blocks until every byte is queued -- with CTS flow control that means it
// blocks while the printer is busy, which is exactly the backpressure the
// raster pipeline wants -- but not past the watchdog: once the stall outlasts
// the timeout and the state flips to BLOCKED, this returns ESP_FAIL with the
// rest of the payload abandoned. The caller owns failing the job; without
// that bound, one job to a stalled printer parks the rip task inside the
// UART driver forever.
esp_err_t serial_out_write(const uint8_t *buf, size_t n);

// Waits for the last byte to leave the shift register. At 9600 baud the FIFO
// can hold a second of data, so a job is not finished when the write returns.
esp_err_t serial_out_flush(uint32_t timeout_ms);

// Discards whatever is still queued in the hardware TX FIFO. For the moment a
// job is failed: without this, a dead job's tail (mid-escape-sequence, up to
// one FIFO) sits queued until paper returns and then prints ahead of the next
// job, leaving the printer in whatever mode the truncation happened in. Call
// only from the writing task with no write in flight.
void serial_out_purge(void);

serial_out_state_t serial_out_state(void);

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
