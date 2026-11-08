// SPDX-License-Identifier: GPL-3.0-or-later
//
// The log the settings page shows (W-1136, docs/superpowers/specs/2026-11-08-log-design.md):
// every esp_log line at info and above, with names taken out, kept in PSRAM across a restart
// and in the first 256 KB of the storage partition across a power cut.
#pragma once
#include <stdint.h>
#include "logbook_core.h"

// Call first in app_main. busy says a job holds the board, so flash stays alone. reset is the
// name of the last reset, for this start's first line.
void logbook_start(int (*busy)(void), const char *reset);

// The saved Wi-Fi network and host name, which the log replaces wherever they appear.
void logbook_set_names(const char *network, const char *host);

// Saves what's new to flash when nothing is printing. The main task, once a second: flash
// calls can't run on a PSRAM stack.
void logbook_tick(void);

// Erases both layers. Saying so is the next thing the log holds.
void logbook_clear(void);

unsigned logbook_errors(void);          // error lines since this start
int      logbook_dropped(void);         // lines lost to a busy lock since this start
uint32_t logbook_newest(void);          // the newest line's number

// One reader at a time, for the web route. newest > 0: only that many lines. 0: all after
// `after`. RAM only while busy() says so, because reading flash competes with the print.
void logbook_read_begin(uint32_t after, unsigned newest);
int  logbook_read_next(lb_line_t *out);
