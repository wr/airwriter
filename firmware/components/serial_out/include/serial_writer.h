// SPDX-License-Identifier: GPL-3.0-or-later
//
// The deadline loop of serial_out_write(), pure C so it runs under the host
// tests in tools/rip. serial_out.c binds these ops to the UART driver and the
// watchdog; the tests bind them to a fake.
//
// The contract that keeps a stalled printer from wedging the rip task: every
// op is non-blocking. tx_write is best-effort -- it takes what fits in the
// hardware FIFO and reports the count, and that return value is ground truth.
// (A first cut bounded uart_write_bytes with uart_get_tx_buffer_free_size;
// that probe only tracks the ring item currently in service, so a CTS stall
// mid-job still parked the write inside the driver forever. No ring, no
// probe, nothing to go stale.)
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    void *ctx;
    // Queue up to n bytes into the hardware FIFO without blocking. Returns
    // bytes accepted (0 when the FIFO is full), or negative on driver error.
    int (*tx_write)(void *ctx, const uint8_t *buf, size_t n);
    // The watchdog's verdict: the printer has stalled past the timeout.
    int (*blocked)(void *ctx);
    // Sleep one watchdog period.
    void (*delay)(void *ctx);
} serial_writer_ops_t;

// 0 with every byte queued, or -1 with bytes abandoned because the watchdog
// called the printer blocked or the driver failed. Paced: any call that
// leaves a remainder is followed by one delay, so a busy FIFO is polled at
// the watchdog cadence, never spun on.
int serial_writer_write(const serial_writer_ops_t *ops, const uint8_t *buf, size_t n);
