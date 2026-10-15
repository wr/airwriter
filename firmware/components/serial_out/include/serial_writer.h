// SPDX-License-Identifier: GPL-3.0-or-later
//
// The write loop of serial_out_write(), pure C so it runs under the host
// tests in tools/rip. serial_out.c binds these ops to the UART driver; the
// tests bind them to a fake.
//
// Every op is non-blocking: tx_write takes what fits in the hardware FIFO and
// reports the count, and that return value is ground truth. A printer that
// holds HSKo false (paper out, SELECT, powered off) simply stops the FIFO
// draining; the loop waits it out and gives up only after stall_limit delays
// in a row with nothing accepted. Nothing is remembered across calls, so every
// write -- including the first one to a printer that has been off for an
// hour -- gets the whole limit.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    void *ctx;
    // Queue up to n bytes into the hardware FIFO without blocking. Returns
    // bytes accepted (0 when the FIFO is full), or negative on driver error.
    int (*tx_write)(void *ctx, const uint8_t *buf, size_t n);
    // Sleep one poll period.
    void (*delay)(void *ctx);
    // The job was cancelled: give up now rather than waiting out a paused
    // printer (Cancel-Job on paper-out must free it at once). May be NULL.
    int (*aborted)(void *ctx);
    // Consecutive delays with nothing accepted before the write gives up.
    // 0 = never.
    unsigned stall_limit;
} serial_writer_ops_t;

// 0 with every byte queued, or -1 with bytes abandoned because the FIFO took
// nothing for stall_limit delays in a row, the job was aborted, or the driver
// failed. Paced: any
// call that leaves a remainder is followed by one delay, never a hot retry.
int serial_writer_write(const serial_writer_ops_t *ops, const uint8_t *buf, size_t n);
