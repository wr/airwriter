// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the printer_state reducer: every row of the spec's state
// table, each timer edge, priority between overlapping states, sleep, and the
// uint32 millisecond wrap. docs/superpowers/specs/2026-10-14-status-language-design.md
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "printer_state_core.h"

static const ps_timing_t TM = PS_TIMING_DEFAULT;

// Booted at t0, joining, a ready printer, Wi-Fi up a second later.
static void up_and_ready(ps_t *ps, uint32_t t0)
{
    ps_init(ps, &TM, t0);
    ps_post(ps, PS_EV_WIFI_JOINING, t0);
    ps_post(ps, PS_EV_LINE_READY, t0);
    ps_post(ps, PS_EV_WIFI_UP, t0 + 1000);
}

static ps_state_t st(ps_t *ps, uint32_t t) { return ps_update(ps, t).state; }

int main(void)
{
    ps_t ps;
    ps_view_t v;

    // Booting lasts boot_ms, then the Wi-Fi state shows.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    assert(st(&ps, 0) == PS_BOOTING);
    assert(st(&ps, 2999) == PS_BOOTING);
    assert(st(&ps, 3000) == PS_JOINING);
    ps_post(&ps, PS_EV_LINE_READY, 3100);
    ps_post(&ps, PS_EV_WIFI_UP, 3200);
    assert(st(&ps, 3200) == PS_READY);

    // Idle and not ready: still ready inside the debounce, then offline; back on, ready.
    up_and_ready(&ps, 0);
    st(&ps, 4000);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
    assert(st(&ps, 12999) == PS_READY);
    assert(st(&ps, 13000) == PS_OFFLINE);
    ps_post(&ps, PS_EV_LINE_READY, 20000);
    assert(st(&ps, 20000) == PS_READY);

    // A job. HSKo chatters on every line, so a 29 s stall is still printing;
    // 30 s asks for attention; the line returning resumes; done is ready.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    assert(st(&ps, 5000) == PS_PRINTING);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
    assert(st(&ps, 39999) == PS_PRINTING);
    assert(st(&ps, 40000) == PS_ATTENTION);
    ps_post(&ps, PS_EV_LINE_READY, 50000);
    assert(st(&ps, 50000) == PS_PRINTING);
    ps_post(&ps, PS_EV_JOB_DONE, 60000);
    assert(st(&ps, 60000) == PS_READY);

    // The printer still busy for a moment after the last byte is not offline.
    ps_post(&ps, PS_EV_LINE_NOT_READY, 60100);
    assert(st(&ps, 62100) == PS_READY);
    ps_post(&ps, PS_EV_LINE_READY, 62200);
    assert(st(&ps, 70000) == PS_READY);

    // A job sent to a printer that has been off a while asks for attention at once.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 5000);
    assert(st(&ps, 45000) == PS_OFFLINE);
    ps_post(&ps, PS_EV_JOB_START, 45000);
    assert(st(&ps, 45000) == PS_ATTENTION);

    // A failed job holds for failed_hold_ms; the next job clears it at once.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    ps_post(&ps, PS_EV_JOB_FAILED, 6000);
    assert(st(&ps, 6000) == PS_FAILED);
    assert(st(&ps, 65999) == PS_FAILED);
    assert(st(&ps, 66000) == PS_READY);
    ps_post(&ps, PS_EV_JOB_START, 70000);
    ps_post(&ps, PS_EV_JOB_FAILED, 71000);
    ps_post(&ps, PS_EV_JOB_START, 72000);
    assert(st(&ps, 72000) == PS_PRINTING);

    // Wi-Fi lost mid-job shows joining; a failure while down shows once back up.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    ps_post(&ps, PS_EV_WIFI_DOWN, 6000);
    assert(st(&ps, 6000) == PS_JOINING);
    ps_post(&ps, PS_EV_JOB_FAILED, 7000);
    ps_post(&ps, PS_EV_WIFI_UP, 9000);
    assert(st(&ps, 9000) == PS_FAILED);

    // Setup outranks everything below booting; phone_joined follows the station count.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_SETUP_AP, 0);
    v = ps_update(&ps, 5000);
    assert(v.state == PS_SETUP && !v.phone_joined);
    ps_set_ap_stations(&ps, 1);
    assert(ps_update(&ps, 5100).phone_joined);
    ps_set_ap_stations(&ps, 0);
    assert(!ps_update(&ps, 5200).phone_joined);

    // since_ms counts from entry into the state.
    up_and_ready(&ps, 0);
    st(&ps, 4000);                                  // READY from 4000
    assert(ps_update(&ps, 4500).since_ms == 500);

    // greeted: Ready entered straight from Joining or Booting opens with the
    // Wi-Fi-connected moment; Ready after a job or after offline does not.
    up_and_ready(&ps, 0);
    assert(ps_update(&ps, 4000).greeted);           // BOOTING -> READY
    ps_post(&ps, PS_EV_JOB_START, 5000);
    st(&ps, 5000);
    ps_post(&ps, PS_EV_JOB_DONE, 6000);
    assert(!ps_update(&ps, 6000).greeted);          // PRINTING -> READY
    ps_post(&ps, PS_EV_WIFI_DOWN, 7000);
    st(&ps, 7000);
    ps_post(&ps, PS_EV_WIFI_UP, 8000);
    assert(ps_update(&ps, 8000).greeted);           // JOINING -> READY
    ps_post(&ps, PS_EV_LINE_NOT_READY, 9000);
    st(&ps, 12000);                                  // OFFLINE
    ps_post(&ps, PS_EV_LINE_READY, 13000);
    assert(!ps_update(&ps, 13000).greeted);         // OFFLINE -> READY

    // Sleep: ready with no activity sleeps after sleep_after_ms; activity wakes
    // it; a change of state wakes it; offline sleeps too; printing never does.
    up_and_ready(&ps, 0);
    st(&ps, 4000);
    assert(!ps_update(&ps, 303999).asleep);
    v = ps_update(&ps, 304000);
    assert(v.asleep && v.asleep_ms == 0);
    assert(ps_update(&ps, 314000).asleep_ms == 10000);
    ps_post(&ps, PS_EV_ACTIVITY, 320000);
    assert(!ps_update(&ps, 320000).asleep);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 330000);
    assert(st(&ps, 333000) == PS_OFFLINE);
    assert(!ps_update(&ps, 632999).asleep);
    assert(ps_update(&ps, 633000).asleep);
    ps_post(&ps, PS_EV_LINE_READY, 640000);
    ps_post(&ps, PS_EV_JOB_START, 640000);
    assert(!ps_update(&ps, 1040000).asleep);

    // The 49-day wrap: timers measured across zero still work.
    {
        uint32_t t0 = 0xFFFFFFFFu - 1000u;
        up_and_ready(&ps, t0);
        st(&ps, t0 + 4000u);
        ps_post(&ps, PS_EV_LINE_NOT_READY, t0 + 5000u);
        assert(st(&ps, t0 + 7999u) == PS_READY);
        assert(st(&ps, t0 + 8000u) == PS_OFFLINE);
    }

    assert(strcmp(ps_name(PS_READY), "ready") == 0);
    assert(strcmp(ps_name(PS_OFFLINE), "offline") == 0);
    assert(strcmp(ps_name(PS_ATTENTION), "attention") == 0);

    printf("test_printer_state: ok\n");
    return 0;
}
