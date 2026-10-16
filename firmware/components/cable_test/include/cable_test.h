// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cable tester (W-1055), ESP side. With one mini-DIN-8 cable from J2 (the
// printer port) to J4 (LocalTalk), it tells Apple's crossover -- the only
// cable that works with the ImageWriter II -- from anything else. The line
// code and the verdict are cable_core.h's; this file borrows the printer
// port's pins for RMT, and TashTalk for the LocalTalk side: the running
// node's link through localtalk.h, or UART2 directly while the node is off.
// When TashTalk isn't there to answer, the verdict says so instead of
// blaming the cable (W-1063).
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "cable_core.h"

typedef struct {
    int tx_pin, rx_pin;     // the printer port: UART1 TX into U2's driver, RX from its receiver
    int invert_tx;          // as serial_out has it, so the borrowed TX idles where the UART left it
    // TashTalk's UART, for when the LocalTalk node is not running
    int lt_uart, lt_tx, lt_rx, lt_cts, lt_mclr;
} cable_test_cfg_t;

typedef struct {
    ct_verdict_t verdict;
    int          back_ok;   // J4 -> J2
    int          fwd_ok;    // J2 -> J4, tried only when back_ok
    esp_err_t    err;       // ESP_OK, or why the test could not run
} cable_result_t;

void cable_test_init(const cable_test_cfg_t *cfg);

// Runs the test: 30 ms with a crossover, up to ~400 ms without (a fail on
// our own TashTalk link is retried once after resetting TashTalk), ~100 ms
// with TashTalk silent (then U7's device ID is read over ICSP, unless the
// node is running). The caller
// holds the job lock, so nothing writes to the printer port while its pins
// are borrowed. J2 only listens until a frame from J4 has come back through
// the cable, so a printer on J2 never sees a test frame.
// both: try J2 -> J4 even when J4 -> J2 failed (a bench check, with no printer on J2).
void cable_test_run(cable_result_t *r, int both);

void cable_result_json(const cable_result_t *r, char *buf, size_t cap);

// 1 while a test holds UART2 itself (the node off): starting the node then
// would find the driver taken. Never longer than ~150 ms.
int  cable_test_lt_busy(void);
