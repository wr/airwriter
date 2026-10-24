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
static void up_and_ready(ps_t *ps, uint64_t t0)
{
    ps_init(ps, &TM, t0);
    ps_post(ps, PS_EV_WIFI_JOINING, t0);
    ps_post(ps, PS_EV_LINE_READY, t0);
    ps_port_line(ps, 1, t0);
    ps_post(ps, PS_EV_WIFI_UP, t0 + 1000);
}

static ps_state_t st(ps_t *ps, uint64_t t) { return ps_update(ps, t).state; }

int main(void)
{
    ps_t ps;
    ps_view_t v;

    // W-1049 quiet power-on: the Spin is all a normal power-on shows. It runs
    // until Wi-Fi is up and the printer line has held still, then goes out
    // (boot_out_ms) straight into the state. A fast boot still ends at 3.0 s.
    up_and_ready(&ps, 0);
    assert(st(&ps, 2999) == PS_BOOTING);
    v = ps_update(&ps, 3000);
    assert(v.state == PS_READY && v.prev == PS_BOOTING && !v.greeted);

    // The usual power-on: the ImageWriter busy for ~3.5 s after the AirWriter
    // powers up (measured on board 1, W-1039), Wi-Fi up at 6 s. The Spin
    // alone, going out from 6 s, then Ready: no Joining, no green, no printer
    // gone or back.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    for (uint64_t t = 0; t < 7300; t += 50) {
        if (t == 3500) ps_post(&ps, PS_EV_LINE_READY, t);
        if (t == 6000) ps_post(&ps, PS_EV_WIFI_UP, t);
        v = ps_update(&ps, t);
        assert(v.state == PS_BOOTING);
        assert(v.spin_out == (t >= 6000));
        if (t >= 6000) assert(v.spin_out_ms == t - 6000);
    }
    v = ps_update(&ps, 7300);
    assert(v.state == PS_READY && v.prev == PS_BOOTING && !v.greeted);

    // No printer: the line never comes up. The Spin waits out the offline
    // settle (10 s from power-on), then the embers come straight up.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    for (uint64_t t = 0; t < 11300; t += 50) {
        if (t == 5000) ps_post(&ps, PS_EV_WIFI_UP, t);
        assert(st(&ps, t) == PS_BOOTING);
    }
    v = ps_update(&ps, 11300);
    assert(v.state == PS_OFFLINE && v.prev == PS_BOOTING);

    // No credentials: Setup as soon as the Spin has had its 3.0 s.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_SETUP_AP, 0);
    assert(st(&ps, 2999) == PS_BOOTING);
    v = ps_update(&ps, 3000);
    assert(v.state == PS_SETUP && !v.join_failed);

    // A failed join of the saved network during the Spin: it goes out, then
    // Setup with join_failed (the two red pulses).
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    ps_post(&ps, PS_EV_LINE_READY, 500);
    assert(st(&ps, 10000) == PS_BOOTING);
    ps_post(&ps, PS_EV_WIFI_SETUP_AP, 12000);
    assert(st(&ps, 12000) == PS_BOOTING);
    v = ps_update(&ps, 13300);
    assert(v.state == PS_SETUP && v.prev == PS_BOOTING && v.join_failed);

    // No Wi-Fi by the cap: the Spin goes out so that Joining shows at 30 s.
    // A join after that is a real one, and the green opens its Ready.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    ps_post(&ps, PS_EV_LINE_READY, 500);
    assert(!ps_update(&ps, 28699).spin_out);
    assert(ps_update(&ps, 28700).spin_out);
    assert(st(&ps, 29999) == PS_BOOTING);
    assert(st(&ps, 30000) == PS_JOINING);
    ps_post(&ps, PS_EV_WIFI_UP, 40000);
    v = ps_update(&ps, 40000);
    assert(v.state == PS_READY && v.prev == PS_JOINING && v.greeted);
    // ... and a join that fails after the cap: Setup from Joining, join_failed.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    ps_post(&ps, PS_EV_LINE_READY, 500);
    assert(st(&ps, 30000) == PS_JOINING);
    ps_post(&ps, PS_EV_WIFI_SETUP_AP, 60000);
    v = ps_update(&ps, 60000);
    assert(v.state == PS_SETUP && v.prev == PS_JOINING && v.join_failed);

    // The green opens the first Ready only on the power-on after Wi-Fi was
    // saved from the web page.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_WIFI_FRESH, 0);
    v = ps_update(&ps, 3000);
    assert(v.state == PS_READY && v.greeted);

    // A job starting during the Spin (a LocalTalk one needs no Wi-Fi)
    // settles it at once.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    assert(!ps_update(&ps, 4999).spin_out);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    assert(ps_update(&ps, 5000).spin_out);
    assert(st(&ps, 6299) == PS_BOOTING);
    assert(st(&ps, 6300) != PS_BOOTING);

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

    // A job started while the line is already down has no printer to wait
    // for -- after an idle spell the printer's buffer is empty, so it is off,
    // unplugged or deselected -- and asks for attention after the offline
    // debounce, not the 30 s mid-page allowance (board 1: a test print with
    // the cable out showed "printing" for 26 s).
    up_and_ready(&ps, 0);
    st(&ps, 4000);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
    ps_post(&ps, PS_EV_JOB_START, 11000);           // 1 s after the cable came out
    assert(st(&ps, 12999) == PS_PRINTING);
    assert(st(&ps, 13000) == PS_ATTENTION);         // 3 s after the line dropped
    // The printer turning up resumes; a later stall in that job gets 30 s.
    ps_post(&ps, PS_EV_LINE_READY, 20000);
    assert(st(&ps, 20000) == PS_PRINTING);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 21000);
    assert(st(&ps, 50999) == PS_PRINTING);
    assert(st(&ps, 51000) == PS_ATTENTION);
    // The next job starts with the line up: the 30 s rule again.
    ps_post(&ps, PS_EV_LINE_READY, 52000);
    ps_post(&ps, PS_EV_JOB_DONE, 53000);
    ps_post(&ps, PS_EV_JOB_START, 54000);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 55000);
    assert(st(&ps, 84999) == PS_PRINTING);
    assert(st(&ps, 85000) == PS_ATTENTION);

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

    // greeted: Ready entered from Joining opens with the Wi-Fi-connected
    // moment; Ready after the Spin (W-1049), a job or offline does not.
    up_and_ready(&ps, 0);
    assert(!ps_update(&ps, 4000).greeted);          // BOOTING -> READY: a quiet power-on
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

    // Past 2^32 ms of uptime (49.7 days) stale stamps stay stale: the boot
    // spin does not replay, an expired failure stays expired, and an old
    // offline stays offline.
    {
        const uint64_t W = 1ull << 32;
        up_and_ready(&ps, 0);
        st(&ps, 4000);
        assert(st(&ps, W + 1000) == PS_READY);
        up_and_ready(&ps, 0);
        ps_post(&ps, PS_EV_JOB_START, 5000);
        ps_post(&ps, PS_EV_JOB_FAILED, 6000);
        assert(st(&ps, 66000) == PS_READY);
        assert(st(&ps, W + 7000) == PS_READY);
        up_and_ready(&ps, 0);
        ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
        assert(st(&ps, 13000) == PS_OFFLINE);
        assert(st(&ps, W + 11000) == PS_OFFLINE);
        assert(ps_update(&ps, W + 11000).since_ms > 0);
    }

    // W-1039: noise on the serial line (a cable replug) can leave the
    // ImageWriter stuck busy with SELECT lit; CTRL-X clears it (bench: stuck
    // 120 s without, ready in 4.1 s with). Kick when idle and not ready for
    // kick_after_ms, then at most every kick_every_ms; never during a job.
    up_and_ready(&ps, 0);
    st(&ps, 4000);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
    ps_port_line(&ps, 0, 10000);
    assert(!ps_want_kick(&ps, 14999));
    assert(ps_want_kick(&ps, 15000));
    ps_kicked(&ps, 15000);
    assert(!ps_want_kick(&ps, 74999));
    assert(ps_want_kick(&ps, 75000));
    ps_kicked(&ps, 75000);
    // The line coming back resets it: a later stall is kicked 5 s in again.
    ps_post(&ps, PS_EV_LINE_READY, 80000);
    ps_port_line(&ps, 1, 80000);
    assert(!ps_want_kick(&ps, 80000));
    ps_post(&ps, PS_EV_LINE_NOT_READY, 100000);
    ps_port_line(&ps, 0, 100000);
    assert(!ps_want_kick(&ps, 104999));
    assert(ps_want_kick(&ps, 105000));
    // W-1088: idle, the box's line is ready while a shared LaserWriter is, but
    // the kick is for the port's ImageWriter: its own line decides (board 1,
    // 2026-10-24: stuck busy after a USB replug, never kicked, the next job
    // stalled at once).
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_LINE_READY, 4000);           // the box: a LaserWriter is ready
    ps_port_line(&ps, 0, 10000);                    // the ImageWriter, stuck busy
    assert(!ps_want_kick(&ps, 14999));
    assert(ps_want_kick(&ps, 15000));
    ps_port_line(&ps, 1, 20000);
    assert(!ps_want_kick(&ps, 90000));
    // Never mid-job: there a low line is paper or a jam, and CTRL-X would
    // erase a line of the page.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 6000);
    ps_port_line(&ps, 0, 6000);
    assert(!ps_want_kick(&ps, 60000));
    // Busy from power-up past the boot settle: kicked like any other stall.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_UP, 2000);
    assert(!ps_want_kick(&ps, 4999));
    assert(ps_want_kick(&ps, 5000));

    // W-1055: a cable test's result shows over the idle states for its hold,
    // wakes the panel, and gives way to a job.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 4000);       // the cable, not a printer, is on J2
    assert(st(&ps, 8000) == PS_OFFLINE);
    ps_cable(&ps, PS_CABLE_PASS, 400000);           // long idle: asleep until now
    v = ps_update(&ps, 400000);
    assert(v.cable == PS_CABLE_PASS && v.cable_ms == 0 && !v.asleep && v.state == PS_OFFLINE);
    v = ps_update(&ps, 400000 + PS_CABLE_MS - 1);
    assert(v.cable == PS_CABLE_PASS && v.cable_ms == PS_CABLE_MS - 1);
    assert(ps_update(&ps, 400000 + PS_CABLE_MS).cable == 0);
    ps_cable(&ps, PS_CABLE_FAIL, 500000);
    assert(ps_update(&ps, 500000 + PS_CABLE_MS - 1).cable == PS_CABLE_FAIL);
    assert(ps_update(&ps, 500000 + PS_CABLE_MS).cable == 0);
    // W-1063: no cable tested (no TashTalk) is shorter: no sweep, two blinks.
    ps_cable(&ps, PS_CABLE_NO_CHIP, 550000);
    assert(ps_update(&ps, 550000 + PS_CABLE_NO_CHIP_MS - 1).cable == PS_CABLE_NO_CHIP);
    assert(ps_update(&ps, 550000 + PS_CABLE_NO_CHIP_MS).cable == 0);
    ps_cable(&ps, PS_CABLE_FAIL, 600000);
    ps_post(&ps, PS_EV_LINE_READY, 600500);         // the printer back on J2
    ps_post(&ps, PS_EV_JOB_START, 601000);
    v = ps_update(&ps, 601000);
    assert(v.state == PS_PRINTING && v.cable == 0);
    // In setup mode too: a new owner may test the cable before joining Wi-Fi.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_SETUP_AP, 0);
    ps_cable(&ps, PS_CABLE_PASS, 10000);
    v = ps_update(&ps, 10000);
    assert(v.state == PS_SETUP && v.cable == PS_CABLE_PASS);

    // W-1049: the view carries the state before this one, which the panel's
    // moments are drawn from.
    up_and_ready(&ps, 0);
    v = ps_update(&ps, 3000);             // Wi-Fi came up during the Spin
    assert(v.state == PS_READY && v.prev == PS_BOOTING);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 4000);
    v = ps_update(&ps, 7000);
    assert(v.state == PS_OFFLINE && v.prev == PS_READY);
    ps_post(&ps, PS_EV_LINE_READY, 8000);
    v = ps_update(&ps, 8000);
    assert(v.state == PS_READY && v.prev == PS_OFFLINE);
    // Still the state before this one, however long this one lasts.
    v = ps_update(&ps, 20000);
    assert(v.state == PS_READY && v.prev == PS_OFFLINE);
    ps_init(&ps, &TM, 0);
    assert(ps_update(&ps, 0).prev == PS_BOOTING);

    // A cancelled job ends as a done one does, and says so until the next
    // job starts.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    assert(st(&ps, 5000) == PS_PRINTING);
    ps_post(&ps, PS_EV_JOB_CANCELLED, 6000);
    v = ps_update(&ps, 6000);
    assert(v.state == PS_READY && v.prev == PS_PRINTING && v.cancelled);
    ps_post(&ps, PS_EV_JOB_START, 7000);
    v = ps_update(&ps, 7000);
    assert(v.state == PS_PRINTING && !v.cancelled);
    ps_post(&ps, PS_EV_JOB_DONE, 8000);
    v = ps_update(&ps, 8000);
    assert(v.state == PS_READY && v.prev == PS_PRINTING && !v.cancelled);

    assert(strcmp(ps_name(PS_READY), "ready") == 0);
    assert(strcmp(ps_name(PS_OFFLINE), "offline") == 0);
    assert(strcmp(ps_name(PS_ATTENTION), "attention") == 0);

    printf("test_printer_state: ok\n");
    return 0;
}
