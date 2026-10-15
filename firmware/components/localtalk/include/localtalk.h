// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LocalTalk node on the board (W-1051): UART2 to TashTalk in U7, the pure
// node in lt_node.c, and what the web UI shows of it. Runs in DIP mode 2, or
// always on a bench build (CONFIG_AIRWRITER_LT_BENCH).
//
// This ticket's scope is being seen: the Chooser lists the board, and a PAP
// connection opens, reads to EOF and closes. The job's bytes are kept in
// PSRAM for GET /api/lt/job -- what the Mac's driver sends -- and are not
// printed yet.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    int         uart_num;
    int         tx_pin;      // LT_UART_TX, TashTalk's RX
    int         rx_pin;      // LT_UART_RX, TashTalk's TX
    int         cts_pin;     // LT_CTS, TashTalk's CTS out; -1 = no flow control
    const char *object;      // NBP object: the queue name
    const char *type;        // NBP type the Chooser looks up
    uint16_t    status;      // Option Card status word to start with (lt_node.h)
} localtalk_cfg_t;

// Starts the node once; later calls do nothing.
esp_err_t localtalk_start(const localtalk_cfg_t *cfg);
int       localtalk_running(void);
// The Option Card status word (LT_IW_* in lt_node.h) for SendStatus.
void      localtalk_set_status(uint16_t status);
// State, counters and the latest lines, for GET /api/lt.
void      localtalk_json(char *buf, size_t cap);
// The last PAP job's bytes as captured (up to the capture size), or NULL.
const uint8_t *localtalk_job(size_t *len);
