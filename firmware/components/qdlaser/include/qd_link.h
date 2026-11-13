// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: J2 as a QuickDraw LaserWriter's link, the only part of the family that touches the
// ESP32's peripherals. The printer drives a clock on HSKi (the port's CTS pin) and takes one
// bit per clock on TxD; the UART takes no outside clock, so GP-SPI2 as a slave, clocked from
// HSKi through the GPIO matrix, shifts out a line image (qd_wire.h) from DMA with no core
// watching. Every chunk is read back off the pad (MOSI fed from TxD) and compared. Replies come
// back on RxD as plain 8N1 at the clock's rate, read by UART1 with flow control off.
//
// While the link is open, serial_out has lent it TxD (serial_out_lend). Each question opens
// and closes it; a job holds it from its first page to its end.
#pragma once
#include <stdint.h>
#include "qd_session.h"

typedef struct { int uart_num, tx_pin, rx_pin, clk_pin, invert_tx; } qd_link_cfg_t;

// Remembers the pins; touches no peripheral.
void qd_link_init(const qd_link_cfg_t *c);
// J2 becomes the printer's at hz: 0, or -1 (no memory, no SPI). Under the job lock. job 0: two
// 1 KB chunks each way, for a question; job 1: two 8 KB chunks, for pages. All of it internal
// DMA-capable RAM, freed at close.
int  qd_link_open(int hz, int job);
// J2 back to serial_out: serial_out_lend(0). The UART stays at the clock's rate, flow control
// off, so the printer's BREAK is still seen at rest; the record's link is the caller's after.
void qd_link_close(void);
int  qd_link_is_open(void);
// put, flush, read, brk, drain, sleep_ms and now_ms; cancelled and state are NULL, the
// caller's to set on a copy.
const qd_ops_t *qd_link_ops(void);

typedef struct {
    uint32_t chunks, bits, bad_bits, stalls;
    int32_t  worst_end_us;    // how late a chunk's end came, against the middle of its tail
} qd_link_stats_t;
void qd_link_stats(qd_link_stats_t *out, int reset);
