// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: the clock on HSKi, found and lost (serial_clock.h).
#include "serial_clock.h"

int serial_clock_window(serial_clock_t *c, int fast)
{
    if (!fast == !c->clocked) {
        c->runs = 0;
        return 0;
    }
    if (++c->runs < (fast ? SERIAL_CLOCK_FOUND_RUNS : SERIAL_CLOCK_GONE_RUNS)) return 0;
    c->runs = 0;
    c->clocked = fast;
    return 1;
}
