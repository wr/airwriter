// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1102: the clock on HSKi is found in two windows and lost only after 2 s of slow ones, so
// a sub-second stop (board 1's 300, 2026-11-13) never reads as the printer gone.
#include "test_util.h"
#include "serial_clock.h"

// Feeds n windows of one verdict; returns how many changes they reported.
static int feed(serial_clock_t *c, int fast, int n)
{
    int changes = 0;
    for (int i = 0; i < n; i++) changes += serial_clock_window(c, fast);
    return changes;
}

static void test_found_in_two_windows(void)
{
    serial_clock_t c = { 0, 0 };
    CHECK_EQ_INT(serial_clock_window(&c, 1), 0);
    CHECK_EQ_INT(c.clocked, 0);
    CHECK_EQ_INT(serial_clock_window(&c, 1), 1);
    CHECK_EQ_INT(c.clocked, 1);
    CHECK_EQ_INT(feed(&c, 1, 50), 0);
}

static void test_a_short_stop_is_not_gone(void)
{
    serial_clock_t c = { 0, 0 };
    feed(&c, 1, 2);
    CHECK_EQ_INT(feed(&c, 0, 8), 0);             // 0.8 s with no clock, as board 1 saw
    CHECK_EQ_INT(c.clocked, 1);
    CHECK_EQ_INT(feed(&c, 1, 1), 0);             // back: the count starts again
    CHECK_EQ_INT(feed(&c, 0, SERIAL_CLOCK_GONE_RUNS - 1), 0);
    CHECK_EQ_INT(c.clocked, 1);
}

static void test_gone_after_two_seconds(void)
{
    serial_clock_t c = { 0, 0 };
    feed(&c, 1, 2);
    CHECK_EQ_INT(feed(&c, 0, SERIAL_CLOCK_GONE_RUNS - 1), 0);
    CHECK_EQ_INT(serial_clock_window(&c, 0), 1);
    CHECK_EQ_INT(c.clocked, 0);
    CHECK_EQ_INT(SERIAL_CLOCK_GONE_RUNS, 20);    // 20 windows of 100 ms
    CHECK_EQ_INT(feed(&c, 0, 50), 0);
}

static void test_a_single_fast_window_is_not_a_clock(void)
{
    serial_clock_t c = { 0, 0 };
    CHECK_EQ_INT(feed(&c, 1, 1), 0);
    CHECK_EQ_INT(feed(&c, 0, 1), 0);
    CHECK_EQ_INT(feed(&c, 1, 1), 0);
    CHECK_EQ_INT(c.clocked, 0);
}

int main(void)
{
    test_found_in_two_windows();
    test_a_short_stop_is_not_gone();
    test_gone_after_two_seconds();
    test_a_single_fast_window_is_not_a_clock();
    TEST_END;
}
