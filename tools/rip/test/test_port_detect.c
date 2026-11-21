// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1179: what each rev 2026.11 socket has plugged in, worked out from what it shows. Socket 0
// is J2 (port 1), socket 1 J4 (port 2); jobs 1 printer, 2 LocalTalk.
#include "test_util.h"
#include "port_detect_core.h"

enum { PR = 1, LT = 2 };

typedef struct {
    pd_t     d;
    pd_in_t  in[PD_SOCKS];
    uint32_t now;
} rig_t;

static void rig(rig_t *r, uint8_t j0, uint8_t j1)
{
    r->now = 1000;
    pd_init(&r->d, 2, r->now);
    for (int i = 0; i < 2; i++)
        r->in[i] = (pd_in_t){ .job = i ? j1 : j0, .can_print = 1, .can_lt = 1 };
}

// Ticks every 500 ms for ms, idle, applying whatever is asked as main would. Returns the
// number of changes and the last one in *last.
static int run(rig_t *r, uint32_t ms, pd_act_t *last)
{
    int changes = 0;
    for (uint32_t t = 0; t < ms; t += 500) {
        r->now += 500;
        pd_act_t a;
        if (pd_tick(&r->d, r->in, r->now, 1, &a)) {
            changes++;
            if (last) *last = a;
            r->in[a.sock].job = a.job;
            if (a.job == LT && r->in[1 - a.sock].job == LT) r->in[1 - a.sock].job = PR;
            pd_applied(&r->d, &a, r->now);
        }
        for (int i = 0; i < 2; i++) r->in[i].frames = r->in[i].junk = 0;
    }
    return changes;
}

static void test_quiet_sockets_keep_their_jobs(void)
{
    rig_t r;
    rig(&r, PR, LT);
    CHECK_EQ_INT(run(&r, 120000, NULL), 0);
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);
}

static void test_a_printer_plugged_into_the_localtalk_socket(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 5000, NULL);
    r.in[1].ready = 1;                           // an ImageWriter, switched on and selected
    pd_act_t a;
    CHECK_EQ_INT(run(&r, 1000, &a), 0);          // not on a blip
    CHECK_EQ_INT(run(&r, 2000, &a), 1);
    CHECK_EQ_INT(a.sock, 1);
    CHECK_EQ_INT(a.job, PR);
    CHECK_EQ_INT(a.why, PD_WHY_HANDSHAKE);
    CHECK_EQ_INT(r.in[0].job, PR);               // two printers now
    CHECK_EQ_INT(run(&r, 60000, NULL), 0);
}

static void test_a_clock_is_a_printer_at_once(void)
{
    rig_t r;
    rig(&r, LT, PR);
    run(&r, 5000, NULL);
    r.in[0].clocked = 1;
    pd_act_t a;
    CHECK_EQ_INT(run(&r, 500, &a), 1);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(a.why, PD_WHY_CLOCK);
}

static void test_a_live_network_is_not_dropped_for_a_handshake(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 5000, NULL);
    r.in[1].ready = 1;
    // A frame every 10 s keeps the network live.
    for (int k = 0; k < 6; k++) {
        r.in[1].frames = 1;
        CHECK_EQ_INT(run(&r, 10000, NULL), 0);
    }
    CHECK_EQ_INT(r.in[1].job, LT);
    // Then nothing for PD_QUIET_MS: the printer gets it.
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_QUIET_MS + 1000, &a), 1);
    CHECK_EQ_INT(a.sock, 1);
    CHECK_EQ_INT(a.job, PR);
}

static void test_localtalk_on_the_printer_socket_is_tried_and_kept(void)
{
    rig_t r;
    rig(&r, PR, LT);                             // the Mac went into J2, the printer into J4
    run(&r, 40000, NULL);                        // J4 quiet: no frames
    pd_act_t a;
    // Junk on J2, three ticks within 5 s: a Chooser's lookups.
    for (int k = 0; k < 2; k++) {
        r.in[0].junk = 2;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
    r.in[0].junk = 1;
    CHECK_EQ_INT(run(&r, 500, &a), 1);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(a.job, LT);
    CHECK_EQ_INT(a.why, PD_WHY_TRAFFIC);
    CHECK_EQ_INT(r.in[0].job, LT);
    CHECK_EQ_INT(r.in[1].job, PR);               // J4 gave LocalTalk up
    // TashTalk hears the Chooser: kept.
    run(&r, 2000, NULL);
    r.in[0].frames = 1;
    CHECK_EQ_INT(run(&r, PD_TRIAL_MS * 2, NULL), 0);
    CHECK_EQ_INT(r.in[0].job, LT);
}

static void test_a_failed_trial_goes_back_and_waits(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 40000, NULL);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_TRIAL_MS + 500, &a), 1);
    CHECK_EQ_INT(a.why, PD_WHY_NO_FRAME);
    CHECK_EQ_INT(a.sock, 1);                     // LocalTalk back to J4, which frees J2
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);
    // More junk on J2 within the backoff changes nothing.
    for (int k = 0; k < 6; k++) {
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
    // After it, a new trial.
    run(&r, PD_BACKOFF_MS, NULL);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);
}

static void test_no_trial_takes_a_live_network(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 5000, NULL);
    for (int k = 0; k < 10; k++) {
        r.in[1].frames = 1;
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);
}

static void test_no_trial_with_a_printer_on_the_line(void)
{
    rig_t r;
    rig(&r, PR, PR);                             // two printers
    run(&r, 5000, NULL);
    r.in[0].ready = 1;                           // J2's printer is on: its noise is its own
    for (int k = 0; k < 6; k++) {
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
    r.in[0].ready = 0;
    r.in[0].clocked = 1;
    for (int k = 0; k < 6; k++) {
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
}

static void test_a_socket_set_to_a_job_is_never_changed(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].fixed = 1;
    r.in[0].fixed = 1;
    run(&r, 5000, NULL);
    r.in[1].ready = 1;
    for (int k = 0; k < 10; k++) {
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
}

static void test_nothing_changes_mid_job(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 5000, NULL);
    r.in[1].ready = 1;
    pd_act_t a;
    for (int k = 0; k < 10; k++) {
        r.now += 500;
        CHECK_EQ_INT(pd_tick(&r.d, r.in, r.now, 0, &a), 0);
    }
    // Idle again: the handshake has been ready all along.
    r.now += 500;
    CHECK_EQ_INT(pd_tick(&r.d, r.in, r.now, 1, &a), 1);
    CHECK_EQ_INT(a.sock, 1);
}

static void test_the_lc_has_one_socket(void)
{
    pd_t d;
    pd_init(&d, 1, 0);
    pd_in_t in[PD_SOCKS] = { { .job = PR, .can_print = 1 } };
    pd_act_t a;
    for (uint32_t t = 500; t < 60000; t += 500) {
        in[0].junk = 1;
        CHECK_EQ_INT(pd_tick(&d, in, t, 1, &a), 0);
    }
}

int main(void)
{
    test_quiet_sockets_keep_their_jobs();
    test_a_printer_plugged_into_the_localtalk_socket();
    test_a_clock_is_a_printer_at_once();
    test_a_live_network_is_not_dropped_for_a_handshake();
    test_localtalk_on_the_printer_socket_is_tried_and_kept();
    test_a_failed_trial_goes_back_and_waits();
    test_no_trial_takes_a_live_network();
    test_no_trial_with_a_printer_on_the_line();
    test_a_socket_set_to_a_job_is_never_changed();
    test_nothing_changes_mid_job();
    test_the_lc_has_one_socket();
    TEST_END;
}
