// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1179: what each rev 2026.11 socket has plugged in, worked out from what it shows. Socket 0
// is J2 (port 1), socket 1 J4 (port 2); jobs 1 printer, 2 LocalTalk. W-1186: each socket by
// itself, so both may do LocalTalk. W-1185: each socket's port sense.
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
    // Quiet for PD_QUIET_MS with the line still up: what sent the frames holds it (a Mac on a
    // direct cable), so LocalTalk stays (review of #51).
    CHECK_EQ_INT(run(&r, PD_QUIET_MS + 5000, NULL), 0);
    CHECK_EQ_INT(r.in[1].job, LT);
    // The cable swapped for a printer's: the line drops and comes up again, and the printer
    // gets the socket once the network has been quiet.
    r.in[1].ready = 0;
    run(&r, 1500, NULL);
    r.in[1].ready = 1;
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
    CHECK_EQ_INT(r.in[1].job, LT);               // J4 keeps LocalTalk: two networks
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
    CHECK_EQ_INT(a.sock, 0);                     // J2 back to the printer job
    CHECK_EQ_INT(a.job, PR);
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);               // J4 untouched
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

// W-1186: a live network on one socket and traffic on the other are two networks: the trial
// goes ahead and the first is left as it is.
static void test_a_trial_leaves_a_live_network_alone(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 5000, NULL);
    pd_act_t a = { -1, 0, PD_WHY_NONE };
    int changes = 0;
    for (int k = 0; k < 4; k++) {
        r.in[1].frames = 1;
        r.in[0].junk = 1;
        changes += run(&r, 1000, &a);
    }
    CHECK_EQ_INT(changes, 1);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(a.why, PD_WHY_TRAFFIC);
    CHECK_EQ_INT(r.in[0].job, LT);
    CHECK_EQ_INT(r.in[1].job, LT);
    CHECK_EQ_INT(pd_heard(&r.d, 1), 1);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 1, r.now), 1);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 0, r.now), 0);      // until it is heard itself
}

// W-1186: two LaserWriters, a port each. Each socket hears its own network and sends on it; a
// printer plugged into one moves that one alone, and with both on LocalTalk no socket prints.
static void test_both_sockets_on_localtalk(void)
{
    rig_t r;
    rig(&r, LT, LT);
    CHECK_EQ_INT(run(&r, 60000, NULL), 0);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 0, r.now), 0);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 1, r.now), 0);
    r.in[0].frames = 1;                          // port 1's LaserWriter switched on
    run(&r, 500, NULL);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 0, r.now), 1);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 1, r.now), 0);
    r.in[1].frames = 1;                          // and port 2's
    run(&r, 500, NULL);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 1, r.now), 1);
    CHECK_EQ_INT(run(&r, 600000, NULL), 0);      // quiet networks keep their sockets
    r.in[0].ready = 1;                           // port 1's cable swapped for an ImageWriter's
    pd_act_t a;
    CHECK_EQ_INT(run(&r, 3000, &a), 1);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(a.job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);
    CHECK_EQ_INT(pd_may_send(&r.d, r.in, 1, r.now), 1);
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

// Review of #51: a Mac on a direct cable that holds its handshake ready looks like a printer on
// the LocalTalk socket once it has been quiet. On the printer job nothing answers there, so its
// traffic, read with the line up, tries LocalTalk again, and its frames keep it.
static void test_a_mac_holding_the_handshake_comes_back(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].ready = 1;
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_QUIET_MS + 3000, &a), 1);
    CHECK_EQ_INT(a.why, PD_WHY_HANDSHAKE);
    CHECK_EQ_INT(r.in[1].job, PR);
    r.in[1].silent = 1;                          // nothing answers ESC ? or the plain question
    for (int k = 0; k < 3; k++) {
        r.in[1].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[1].job, LT);
    run(&r, 1000, NULL);
    r.in[1].frames = 1;
    CHECK_EQ_INT(run(&r, PD_TRIAL_MS * 2, NULL), 0);
    CHECK_EQ_INT(r.in[1].job, LT);
    // A printer that answered on a ready line keeps its socket, whatever noise it makes.
    rig(&r, PR, PR);
    run(&r, 5000, NULL);
    r.in[0].ready = 1;
    r.in[0].silent = 0;
    for (int k = 0; k < 6; k++) {
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
}

static int may(const rig_t *r, int i)
{
    return pd_may_send(&r->d, r->in, i, r->now);
}

// Frames sent into a printer print as junk, selected or not: AppleTalk only where LocalTalk has
// been heard (board 1, 2026-11-21).
static void test_a_localtalk_socket_listens_until_heard(void)
{
    rig_t r;
    rig(&r, PR, LT);                             // nothing known: J4 does LocalTalk
    run(&r, 60000, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
    CHECK_EQ_INT(pd_heard(&r.d, 1), 0);
    CHECK_EQ_INT(may(&r, 0), 0);                 // a printer socket never sends AppleTalk
    r.in[1].frames = 1;                          // a Mac's Chooser, or a LaserWriter switched on
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    CHECK_EQ_INT(pd_heard(&r.d, 1), 1);
    CHECK_EQ_INT(run(&r, 600000, NULL), 0);      // a quiet network keeps it
    CHECK_EQ_INT(may(&r, 1), 1);
}

static void test_a_printer_where_localtalk_was_heard_stops_it_at_once(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].frames = 1;
    run(&r, 40000, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    r.in[1].ready = 1;                           // the cable swapped for an ImageWriter's
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);                 // the next tick, not after PD_READY_MS
    CHECK_EQ_INT(pd_heard(&r.d, 1), 0);
    r.in[1].ready = 0;                           // deselected before the socket changed job
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);                 // still nothing until LocalTalk is heard
    r.in[1].frames = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    // A QuickDraw LaserWriter's clock, the same.
    r.in[1].clocked = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
}

static void test_a_mac_holding_the_handshake_may_send(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].ready = 1;                           // a Mac on a direct cable holds HSKo ready
    run(&r, 1000, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
    r.in[1].frames = 1;                          // and talks
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    CHECK_EQ_INT(r.in[1].job, LT);
}

static void test_heard_before_a_restart_waits_for_the_line(void)
{
    rig_t r;
    rig(&r, PR, LT);
    pd_start(&r.d, 1, LT, 1);
    pd_start(&r.d, 0, PR, 1);                    // a printer socket's is not kept
    CHECK_EQ_INT(pd_heard(&r.d, 0), 0);
    CHECK_EQ_INT(may(&r, 1), 0);                 // before the first tick
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
    run(&r, PD_BOOT_MS - 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);                 // the line showed no printer
    // An ImageWriter plugged in while AirWriter was off shows at the first tick.
    rig(&r, PR, LT);
    pd_start(&r.d, 1, LT, 1);
    r.in[1].ready = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(pd_heard(&r.d, 1), 0);
    r.in[1].ready = 0;
    run(&r, PD_BOOT_MS, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
    // A frame heard since the restart needs no wait.
    rig(&r, PR, LT);
    pd_start(&r.d, 1, LT, 1);
    r.in[1].frames = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
}

static void test_a_trial_sends_once_heard(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 40000, NULL);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);
    CHECK_EQ_INT(may(&r, 0), 0);                 // junk is only a hint
    run(&r, 2000, NULL);
    CHECK_EQ_INT(may(&r, 0), 0);
    r.in[0].frames = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 0), 1);
}

// W-1186: a trial on one socket, kept or failed, leaves what the other has heard.
static void test_a_trial_leaves_what_the_other_heard(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].frames = 1;                          // a LaserWriter on J4, heard, then quiet
    run(&r, PD_QUIET_MS + 5000, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);               // J2 on trial
    CHECK_EQ_INT(r.in[1].job, LT);
    CHECK_EQ_INT(may(&r, 1), 1);                 // the LaserWriter is still reached
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_TRIAL_MS + 500, &a), 1);
    CHECK_EQ_INT(a.why, PD_WHY_NO_FRAME);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);
    CHECK_EQ_INT(may(&r, 1), 1);
    CHECK_EQ_INT(may(&r, 0), 0);
}

static void test_a_socket_set_to_localtalk_sends(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].fixed = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    // Changed from outside (its setting, the cable test): heard again first.
    rig(&r, PR, LT);
    r.in[1].frames = 1;
    run(&r, 1000, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    r.in[0].job = LT;
    r.in[1].job = PR;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 0), 0);
    r.in[0].job = PR;
    r.in[1].job = LT;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
}

// W-1186: an ImageWriter switched off and on makes noise on its line, which may start a trial
// while it is down. Its handshake coming up ends the trial at that check, not 20 s later, and
// so does a QuickDraw LaserWriter's clock; neither waits a backoff out.
static void test_a_printer_showing_ends_a_trial(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 40000, NULL);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);               // J2 on trial, its printer still starting
    CHECK_EQ_INT(run(&r, 2000, NULL), 0);
    r.in[0].ready = 1;                           // and now ready
    pd_act_t a;
    CHECK_EQ_INT(run(&r, 500, &a), 1);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(a.job, PR);
    CHECK_EQ_INT(a.why, PD_WHY_HANDSHAKE);
    CHECK_EQ_INT(r.d.s[0].trial, 0);
    CHECK_EQ_INT(r.in[1].job, LT);
    CHECK_EQ_INT(run(&r, 60000, NULL), 0);       // and it stays a printer port
    // A clock, the same.
    rig(&r, PR, LT);
    run(&r, 40000, NULL);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);
    r.in[0].clocked = 1;
    CHECK_EQ_INT(run(&r, 500, &a), 1);
    CHECK_EQ_INT(a.why, PD_WHY_CLOCK);
    CHECK_EQ_INT(r.in[0].job, PR);
}

// Junk on each socket in turn: two trials at once, each its own. The one that hears a frame is
// kept; the other goes back to the printer job and waits out its own backoff.
static void test_two_trials_at_once(void)
{
    rig_t r;
    rig(&r, PR, PR);
    run(&r, 40000, NULL);
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);               // J2 on trial
    for (int k = 0; k < 3; k++) {
        r.in[1].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[1].job, LT);               // J4 on trial too
    CHECK_EQ_INT(r.in[0].job, LT);
    r.in[0].frames = 1;                          // J2's is a network
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_TRIAL_MS + 500, &a), 1);
    CHECK_EQ_INT(a.why, PD_WHY_NO_FRAME);
    CHECK_EQ_INT(a.sock, 1);
    CHECK_EQ_INT(r.in[1].job, PR);
    CHECK_EQ_INT(r.in[0].job, LT);
    CHECK_EQ_INT(r.d.s[1].trial, 0);
    CHECK_EQ_INT(r.d.s[0].trial, 0);
    CHECK_EQ_INT(may(&r, 0), 1);
    for (int k = 0; k < 6; k++) {                // J4 waits out its backoff
        r.in[1].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
}

// ------------------------------------------------------------------ W-1185: the port sense

enum { NONE = PD_SENSE_NONE, CLEAR = PD_SENSE_CLEAR, PRINTER = PD_SENSE_PRINTER };

// A LaserWriter switched on before it was cabled in: the socket hears nothing, but its sense
// is held clear, so it sends at once and the LaserWriter is found.
static void test_a_clear_sense_sends_unheard(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].sense = CLEAR;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);                 // the handshake and clock get PD_BOOT_MS
    run(&r, PD_BOOT_MS, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    CHECK_EQ_INT(pd_heard(&r.d, 1), 0);
    CHECK_EQ_INT(may(&r, 0), 0);                 // a printer socket never sends, clear or not
    r.in[0].sense = CLEAR;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 0), 0);
    CHECK_EQ_INT(run(&r, 600000, NULL), 0);      // and neither changes job for it
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(r.in[1].job, LT);
    // With no level held yet the socket listens, as without a sense.
    rig(&r, PR, LT);
    run(&r, 60000, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
}

// The handshake and the clock still count with the sense clear: a Mac on a direct cable that
// holds its handshake ready is heard first, as before.
static void test_a_clear_sense_keeps_the_handshake_and_clock(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].sense = CLEAR;
    r.in[1].ready = 1;
    run(&r, PD_BOOT_MS + 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 0);
    r.in[1].frames = 1;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    rig(&r, PR, LT);
    r.in[1].sense = CLEAR;
    run(&r, PD_BOOT_MS, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    r.in[1].clocked = 1;                         // a QuickDraw LaserWriter's clock
    CHECK_EQ_INT(may(&r, 1), 0);
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_READY_MS, &a), 1);
    CHECK_EQ_INT(a.why, PD_WHY_CLOCK);
}

// A deselected ImageWriter plugged into a socket that heard LocalTalk: its sense holds a
// printer, so not another frame, and the socket becomes a printer port once that has held.
static void test_a_sensed_printer_stops_sending_and_takes_the_socket(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].sense = CLEAR;
    r.in[1].frames = 1;
    run(&r, 40000, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    r.in[1].sense = PRINTER;                     // handshake down: deselected
    pd_act_t a;
    CHECK_EQ_INT(run(&r, 500, &a), 0);
    CHECK_EQ_INT(may(&r, 1), 0);                 // the next tick
    CHECK_EQ_INT(pd_heard(&r.d, 1), 0);
    CHECK_EQ_INT(run(&r, PD_READY_MS, &a), 1);   // held PD_READY_MS, quiet or not lately
    CHECK_EQ_INT(a.sock, 1);
    CHECK_EQ_INT(a.job, PR);
    CHECK_EQ_INT(a.why, PD_WHY_SENSE);
    CHECK_EQ_INT(r.in[0].job, PR);
    CHECK_EQ_INT(run(&r, 60000, NULL), 0);       // it stays a printer port
    CHECK_EQ_INT(may(&r, 1), 0);
}

// Frames heard while the sense holds a printer: it is stale (a frame pulses the line, and a
// network busy enough leaves it no gap to hold another level in), so the socket keeps
// LocalTalk, sending nothing. Held low once the frames stop: a printer after all.
static void test_frames_keep_localtalk_against_the_sense(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 5000, NULL);
    r.in[1].sense = PRINTER;
    for (int k = 0; k < 8; k++) {
        r.in[1].frames = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
        CHECK_EQ_INT(may(&r, 1), 0);
    }
    CHECK_EQ_INT(r.in[1].job, LT);
    r.in[1].sense = CLEAR;                       // the line rests between frames
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    r.in[1].sense = PRINTER;
    r.in[1].frames = 1;
    run(&r, 500, NULL);
    pd_act_t a;
    CHECK_EQ_INT(run(&r, PD_READY_MS - 500, NULL), 0);
    CHECK_EQ_INT(run(&r, 500, &a), 1);
    CHECK_EQ_INT(a.sock, 1);
    CHECK_EQ_INT(a.why, PD_WHY_SENSE);
}

// A socket set to LocalTalk is never changed, and never sends into a printer with power.
static void test_a_sensed_printer_on_a_socket_set_to_localtalk(void)
{
    rig_t r;
    rig(&r, PR, LT);
    r.in[1].fixed = 1;
    r.in[1].sense = PRINTER;
    CHECK_EQ_INT(run(&r, 60000, NULL), 0);
    CHECK_EQ_INT(r.in[1].job, LT);
    CHECK_EQ_INT(may(&r, 1), 0);
    r.in[1].sense = CLEAR;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
    r.in[1].sense = NONE;
    run(&r, 500, NULL);
    CHECK_EQ_INT(may(&r, 1), 1);
}

// A printer socket whose sense holds a printer is never tried for LocalTalk, whatever its line
// says, and a trial ends the moment the sense holds one.
static void test_a_sensed_printer_blocks_and_ends_a_trial(void)
{
    rig_t r;
    rig(&r, PR, LT);
    run(&r, 40000, NULL);
    r.in[0].sense = PRINTER;                     // a deselected ImageWriter, switched on
    r.in[0].silent = 1;
    for (int k = 0; k < 6; k++) {
        r.in[0].junk = 1;
        CHECK_EQ_INT(run(&r, 1000, NULL), 0);
    }
    CHECK_EQ_INT(r.in[0].job, PR);
    // Junk while the printer was off starts a trial; switched on, it ends it.
    rig(&r, PR, LT);
    run(&r, 40000, NULL);
    r.in[0].sense = CLEAR;
    for (int k = 0; k < 3; k++) {
        r.in[0].junk = 1;
        run(&r, 1000, NULL);
    }
    CHECK_EQ_INT(r.in[0].job, LT);
    CHECK_EQ_INT(may(&r, 0), 1);                 // clear: the trial's node may look around
    r.in[0].sense = PRINTER;
    pd_act_t a;
    CHECK_EQ_INT(run(&r, 500, &a), 1);
    CHECK_EQ_INT(a.sock, 0);
    CHECK_EQ_INT(a.job, PR);
    CHECK_EQ_INT(a.why, PD_WHY_SENSE);
    CHECK_EQ_INT(r.d.s[0].trial, 0);
    CHECK_EQ_INT(may(&r, 0), 0);
}

// A level counts once it has held PD_SENSE_HOLD_MS with no edge and the node quiet, looked at
// every PD_SENSE_LOOK_MS.
static void test_the_sense_holds_a_level(void)
{
    pd_sense_t s = { 0 };
    uint32_t t = 5000;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), NONE);       // the first look starts a run
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), CLEAR);
    // A printer plugged in: one edge, then low.
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 0, 1, 0, t), CLEAR);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 0, 0, 0, t), PRINTER);
    // It answers a question: edges, and the line back low. Nothing changes.
    for (int k = 0; k < 5; k++) {
        t += PD_SENSE_LOOK_MS;
        CHECK_EQ_INT(pd_sense_look(&s, 0, 1, 0, t), PRINTER);
    }
    // Unplugged with no edge counted (the look between): the level alone restarts the run.
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), PRINTER);
    t += PD_SENSE_HOLD_MS - 1;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), PRINTER);    // a late look early is not enough
    t += 1;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), CLEAR);
}

// LocalTalk frames pulse the line: every look with an edge restarts the run, so a busy network
// holds nothing new, and a low sampled mid-frame never reads as a printer.
static void test_frames_never_hold_the_sense(void)
{
    pd_sense_t s = { 0 };
    uint32_t t = 5000;
    pd_sense_look(&s, 1, 0, 0, t);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), CLEAR);
    for (int k = 0; k < 100; k++) {              // a frame in every window, caught low or high
        t += PD_SENSE_LOOK_MS;
        CHECK_EQ_INT(pd_sense_look(&s, k & 1, 1, 0, t), CLEAR);
    }
    pd_sense_t n = { 0 };                        // and from boot, nothing at all
    for (int k = 0; k < 100; k++) {
        t += PD_SENSE_LOOK_MS;
        CHECK_EQ_INT(pd_sense_look(&n, 0, 1, 0, t), NONE);
    }
}

// The socket's own node sending: its frames may reach the line through a LocalTalk box, so a
// high is not taken until PD_SENSE_TX_MS after its last frame. A low holds regardless: its
// frames would leave edges, and a printer plugged in mid-burst must stop them.
static void test_the_sense_waits_out_its_own_frames(void)
{
    pd_sense_t s = { 0 };
    uint32_t t = 5000, tx = t;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), NONE);
    for (int k = 0; k < 6; k++) {                // high, steady, while it sends: not clear yet
        t += PD_SENSE_LOOK_MS;
        tx = t - 10;
        CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), NONE);
    }
    t = tx + PD_SENSE_TX_MS - 1;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), NONE);
    t = tx + PD_SENSE_TX_MS;                     // quiet from here: a run starts at the last look
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), NONE);
    t += PD_SENSE_HOLD_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), CLEAR);
    // A printer plugged in while the node keeps sending: held within a look or two.
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 0, 0, t - 5, t), CLEAR);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 0, 0, t - 5, t), PRINTER);
}

// A frame queued in the moment after the look read the time is sending, not 49 days ago; one
// queued 2^31 ms ago is long ago, not in the future.
static void test_the_sense_reads_its_frames_times_right(void)
{
    pd_sense_t s = { 0 };
    uint32_t t = 5000;
    pd_sense_look(&s, 0, 0, 0, t);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 0, 0, 0, t), PRINTER);
    t += PD_SENSE_LOOK_MS;                       // unplugged: high from here, steady
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, 0, t), PRINTER);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, t + 1, t), PRINTER);     // queued just after
    t += PD_SENSE_LOOK_MS;
    uint32_t tx = t - PD_SENSE_LOOK_MS + 1;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), PRINTER);
    t = tx + PD_SENSE_TX_MS + PD_SENSE_HOLD_MS;
    CHECK_EQ_INT(pd_sense_look(&s, 1, 0, tx, t), CLEAR);
    // A frame long ago, past half the clock's range: the sense holds as if never sent.
    pd_sense_t o = { 0 };
    t = 0x90000000u;
    tx = t - 0x80000000u;
    pd_sense_look(&o, 1, 0, tx, t);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&o, 1, 0, tx, t), CLEAR);
    tx = t - 0xF0000000u;
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&o, 0, 1, tx, t), CLEAR);
    t += PD_SENSE_LOOK_MS;
    CHECK_EQ_INT(pd_sense_look(&o, 0, 0, tx, t), PRINTER);
}

// The cable test sends into a socket held clear, or with no sense, but into one holding a
// printer only when the test cable shows itself.
static void test_the_cable_test_never_sends_into_a_printer(void)
{
    CHECK_EQ_INT(pd_test_may_send(NONE, 0), 1);
    CHECK_EQ_INT(pd_test_may_send(CLEAR, 0), 1);
    CHECK_EQ_INT(pd_test_may_send(PRINTER, 0), 0);
    CHECK_EQ_INT(pd_test_may_send(PRINTER, 1), 1);
}

int main(void)
{
    test_quiet_sockets_keep_their_jobs();
    test_a_printer_plugged_into_the_localtalk_socket();
    test_a_clock_is_a_printer_at_once();
    test_a_live_network_is_not_dropped_for_a_handshake();
    test_localtalk_on_the_printer_socket_is_tried_and_kept();
    test_a_failed_trial_goes_back_and_waits();
    test_a_trial_leaves_a_live_network_alone();
    test_both_sockets_on_localtalk();
    test_no_trial_with_a_printer_on_the_line();
    test_a_mac_holding_the_handshake_comes_back();
    test_a_socket_set_to_a_job_is_never_changed();
    test_nothing_changes_mid_job();
    test_the_lc_has_one_socket();
    test_a_localtalk_socket_listens_until_heard();
    test_a_printer_where_localtalk_was_heard_stops_it_at_once();
    test_a_mac_holding_the_handshake_may_send();
    test_heard_before_a_restart_waits_for_the_line();
    test_a_trial_sends_once_heard();
    test_a_trial_leaves_what_the_other_heard();
    test_a_socket_set_to_localtalk_sends();
    test_a_printer_showing_ends_a_trial();
    test_two_trials_at_once();
    test_a_clear_sense_sends_unheard();
    test_a_clear_sense_keeps_the_handshake_and_clock();
    test_a_sensed_printer_stops_sending_and_takes_the_socket();
    test_frames_keep_localtalk_against_the_sense();
    test_a_sensed_printer_on_a_socket_set_to_localtalk();
    test_a_sensed_printer_blocks_and_ends_a_trial();
    test_the_sense_holds_a_level();
    test_frames_never_hold_the_sense();
    test_the_sense_waits_out_its_own_frames();
    test_the_sense_reads_its_frames_times_right();
    test_the_cable_test_never_sends_into_a_printer();
    TEST_END;
}
