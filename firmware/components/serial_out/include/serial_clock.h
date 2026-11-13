// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: whether a clock runs on HSKi (a QuickDraw LaserWriter switched on), judged from
// serial_out's edge-count windows, one every 100 ms. Pure C so the host tests in tools/rip
// run it. A clock is found after two fast windows in a row and gone only after
// SERIAL_CLOCK_GONE_RUNS slow ones: board 1's Personal LaserWriter 300 stopped its clock for
// under a second, untouched, three times in an afternoon (2026-11-13), and each showed on the
// phones as Offline and back. A printer switched off still reads gone in about 2 s.
#pragma once

enum { SERIAL_CLOCK_FOUND_RUNS = 2, SERIAL_CLOCK_GONE_RUNS = 20 };

typedef struct { int clocked, runs; } serial_clock_t;

// One window's verdict (fast: its rate at or above the clock threshold). 1 when the clock was
// found or lost with this window (c->clocked is the new state), else 0.
int serial_clock_window(serial_clock_t *c, int fast);
