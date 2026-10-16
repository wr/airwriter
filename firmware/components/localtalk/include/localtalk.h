// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LocalTalk node on the board (W-1051): UART2 to TashTalk in U7, the pure
// node in lt_node.c, and what the web UI shows of it. Runs in DIP mode 2, or
// always on a bench build (CONFIG_AIRWRITER_LT_BENCH).
//
// A PAP job is the Mac driver's own ImageWriter stream (W-1053): spooled in
// a 16 KB buffer and written to the printer through cfg->printer by a print
// task, so a paused printer holds the Mac (no next SendData until the spool
// has room) and never the LocalTalk task. The last job's bytes are also kept
// in PSRAM for GET /api/lt/job.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// The printer, as main.c owns it. All run on the print task except begin.
typedef struct {
    void *ctx;
    // Take the printer for a job without waiting (the LocalTalk task): 0, or
    // nonzero when busy (an IPP job, a test page), which the Mac is told.
    int  (*begin)(void *ctx);
    // Write job bytes, blocking while the printer is paused. 0, or nonzero
    // when the printer gave up (stalled past the limit): the job is aborted.
    int  (*write)(void *ctx, const uint8_t *d, size_t n);
    // The job is over and its bytes are written or dropped: normal is 1 when
    // the Mac ended it (CloseConn, EOF), 0 when it was abandoned or aborted.
    // Returns 1 when the whole job reached the printer.
    int  (*end)(void *ctx, int normal);
} localtalk_printer_t;

typedef struct {
    int         uart_num;
    int         tx_pin;      // LT_UART_TX, TashTalk's RX
    int         rx_pin;      // LT_UART_RX, TashTalk's TX
    int         cts_pin;     // LT_CTS, TashTalk's CTS out; -1 = no flow control
    const char *object;      // NBP object: the queue name
    const char *type;        // NBP type the Chooser looks up
    uint16_t    status;      // Option Card status word to start with (lt_node.h)
    localtalk_printer_t printer;   // NULL begin: jobs are captured and dropped
} localtalk_cfg_t;

// Starts the node once; later calls do nothing.
esp_err_t localtalk_start(const localtalk_cfg_t *cfg);
int       localtalk_running(void);
// The Option Card status word (LT_IW_* in lt_node.h) for SendStatus.
void      localtalk_set_status(uint16_t status);
// State, counters and the latest lines, for GET /api/lt.
void      localtalk_json(char *buf, size_t cap);
// A line for /api/lt's event log (and the console), from any task.
void      localtalk_note(const char *line);
// Bench check: an AEP echo request to `node`; the reply shows in the log.
void      localtalk_echo(uint8_t node);
// The last PAP job's bytes as captured (up to the capture size), or NULL.
const uint8_t *localtalk_job(size_t *len);

// The cable tester (W-1055) borrows the running node's link: every frame
// TashTalk relays, FCS checked and stripped, also goes to `tap` (on the
// LocalTalk task), and localtalk_send_raw queues a frame for TashTalk as the
// node's own are. A NULL tap removes it.
void localtalk_set_tap(void (*tap)(void *ctx, const uint8_t *f, size_t n), void *ctx);
void localtalk_send_raw(const uint8_t *f, size_t n);
