// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the idle printer port is asked, and when (W-1103, design "The port"; W-1116, design
// "What the port is asked"): the pure policy behind the board's port task, and the bookkeeping
// between its calls. The port holds a record per model; the one that answered last is the
// current record, whose kind the policy is given, and what the list holds (any StyleWriter, an
// original ImageWriter, an original StyleWriter) sets the order of the questions. A StyleWriter
// that answers is asked its status every 10 s and nothing else; a port whose printer is not
// answering is asked the plain question every 5 s; ESC ? waits for a line-up (or a look with the
// line ready), after the plain question where the port is StyleWriter-first; the ImageWriter's
// CTRL-X kick goes only to a port whose current record is not a StyleWriter and whose plain
// question got nothing. A Something else record stops everything but the person's own look; an
// act the board wants repeated (`confirm`) goes first. The last part runs the whole loop for ten
// minutes against scripted printers, every record against every printer, and through scripts of
// swaps with the list kept as the board keeps it: no question loops, no StyleWriter hears ESC ?
// while it would answer the plain question, or the kick at all, and every printer plugged in is
// answering within seconds.
#include "test_util.h"
#include "port_policy.h"
#include "printers_core.h"

#define SEC 1000u   // ms

// A current record of `kind`; a StyleWriter record is listed when it is the current one.
static port_in_t base(int kind)
{
    port_in_t i;
    memset(&i, 0, sizeof i);
    i.kind = kind;
    i.sw_listed = kind == PR_KIND_STYLEWRITER;
    i.line_ready = 1;
    i.now_ms = 1000u * SEC;
    i.last_plain_ms = i.now_ms;
    i.last_status_ms = i.now_ms;
    return i;
}

static port_in_t at(port_in_t i, uint32_t since_plain, uint32_t since_status)
{
    i.last_plain_ms = i.now_ms - since_plain;
    i.last_status_ms = i.now_ms - since_status;
    return i;
}

static const int KINDS[] = { -1, PR_KIND_UNKNOWN, PR_KIND_IMAGEWRITER, PR_KIND_LASERWRITER,
                             PR_KIND_AT_IMAGEWRITER, PR_KIND_STYLEWRITER };
#define N_KINDS ((int)(sizeof KINDS / sizeof KINDS[0]))

// The caller as the board runs it, with a printer that never answers but a StyleWriter
// record's, which keeps answering when `answering` is set: every 500 ms it calls once, does
// what it is told and keeps the books. counts[] is how often each action came.
typedef struct { int counts[6]; uint32_t at[6][64]; } tally_t;

static tally_t run(port_in_t in, uint32_t from_ms, uint32_t to_ms)
{
    tally_t t;
    memset(&t, 0, sizeof t);
    int sw_answers = in.kind == PR_KIND_STYLEWRITER && in.answering;
    for (uint32_t ms = from_ms; ms < to_ms; ms += 500) {
        in.now_ms = ms;
        port_tick(&in);
        port_act_t a = port_policy(&in);
        if (t.counts[a] < 64) t.at[a][t.counts[a]] = ms;
        t.counts[a]++;
        int heard = sw_answers && (a == PA_PLAIN || a == PA_STATUS) ? PR_HEARD_STYLEWRITER : PR_HEARD_NONE;
        port_after(&in, a, heard, heard != PR_HEARD_NONE);
    }
    return t;
}

// W-1116: what the list holds decides the order, not the current record alone.
static int sw_first_of(const port_in_t *in)
{
    return in->sw_listed || in->heard == PR_HEARD_STYLEWRITER || in->iw1_listed;
}

// Every combination of the flags, three times for the clock: due for neither question, for
// the status, for the plain one. What the policy must never do, whatever else is true.
static void test_invariants(void)
{
    for (int k = 0; k < N_KINDS; k++)
    for (unsigned f = 0; f < 32768; f++)
    for (int heard = PR_HEARD_NONE; heard <= PR_HEARD_STYLEWRITER; heard++)
    for (int due = 0; due < 3; due++) {
        port_in_t in = base(KINDS[k]);
        in.said_other = f & 1;
        in.answering = (f >> 1) & 1;
        in.line_ready = (f >> 2) & 1;
        in.line_up_edge = (f >> 3) & 1;
        in.look = (f >> 4) & 1;
        in.want_kick = (f >> 5) & 1;
        in.plain_unanswered = (f >> 6) & 1;
        in.plain_asked = (f >> 7) & 1;
        in.event_plain = (f >> 8) & 1;
        in.iw1_listed = (f >> 9) & 1;
        in.sw_listed = (f >> 10) & 1;
        in.sw1_listed = (f >> 11) & 1;
        in.look_person = (f >> 12) & 1;
        in.full_due = (f >> 13) & 1;
        in.confirm = (f >> 14) & 1 ? (port_act_t)(1 + f % 5) : PA_NONE;
        in.heard = heard;
        if (due == 1) in.last_status_ms = in.now_ms - 10 * SEC;
        if (due == 2) in.last_plain_ms = in.now_ms - 5 * SEC;
        port_act_t a = port_policy(&in);
        int sw = KINDS[k] == PR_KIND_STYLEWRITER;
        int iw = KINDS[k] == PR_KIND_IMAGEWRITER;
        int answers = KINDS[k] >= 0 && in.answering;
        int event = in.line_up_edge || (in.look && in.line_ready);
        int person_look = in.look && in.look_person;

        // An act the board wants repeated goes first, whatever else is true.
        if (in.confirm != PA_NONE) { CHECK_EQ_INT(a, in.confirm); continue; }
        // Something else: nothing but the person's own look, and only its questions.
        if (in.said_other && !person_look) CHECK_EQ_INT(a, PA_NONE);
        if (in.said_other) CHECK(a == PA_NONE || a == PA_PLAIN || a == PA_ESCQ || a == PA_STATUS);
        // The kick: wanted, after a plain question since the line went down that got
        // nothing, never while the current record is a StyleWriter.
        if (a == PA_KICK) CHECK(in.want_kick && !sw && in.plain_asked && in.plain_unanswered);
        // The full sequence: only with an original StyleWriter record listed, at start-up or
        // after the person's look, only once that look's questions got nothing, and never
        // while the port's printer answers.
        if (a == PA_FULL) CHECK(in.full_due && in.sw1_listed && !answers && !event && !in.look);
        // The plain question only for a reason: a line-up or a look that takes it first, a
        // look with the line down, the 5 s cadence, or a wanted kick with none since the
        // line went down; never to a port that answers, but an event's on a port that is
        // StyleWriter-first.
        if (a == PA_PLAIN)
            CHECK((event && sw_first_of(&in) && !in.event_plain) ||
                  (!answers && (in.look || due == 2 || (in.want_kick && !in.plain_asked && !sw))));
        if (a == PA_PLAIN && answers) CHECK(event && sw_first_of(&in) && !sw);
        // ESC ? only on a line-up or a look with the line up; first only where the port is not
        // StyleWriter-first, else once the plain question for it got nothing.
        if (a == PA_ESCQ) CHECK(!(sw && answers) && event && (!sw_first_of(&in) || in.event_plain));
        if (a == PA_STATUS) CHECK(sw && answers);
        if (sw && answers) CHECK(a == PA_NONE || a == PA_STATUS);
        // An ImageWriter that answers is never asked the plain question on its own.
        if (iw && in.answering && !event) CHECK(a != PA_PLAIN && a != PA_STATUS);
        // A port that is not answering is asked the plain question on the cadence whatever
        // answered last: an ImageWriter record whose port a StyleWriter answered is no longer
        // quiet (W-1116; the "changed" notice it waited for is gone).
        if (!in.said_other && !answers && due == 2 && !event && !in.look &&
            !(in.full_due && in.sw1_listed))
            CHECK_EQ_INT(a, PA_PLAIN);
    }
}

static void test_said_other(void)
{
    for (int k = 0; k < N_KINDS; k++) {
        port_in_t in = base(KINDS[k]);
        in.said_other = 1;
        in.answering = 1;
        in.line_up_edge = 1;
        in.look = 1;                 // the board's own look, not the person's
        in.want_kick = 1;
        in.plain_unanswered = 1;
        in.plain_asked = 1;
        in.full_due = in.sw1_listed = 1;
        in = at(in, 600 * SEC, 600 * SEC);
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
        in.look = 0;
        in.look_person = 1;          // a stale flag with no look pending
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
    }
    port_in_t in = base(-1);
    in.said_other = 1;
    tally_t t = run(in, 0, 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_NONE], 1200);
}

// W-1116: Look for Printers, the person's own request, asks once on a port set to Something
// else: the look's questions as on any port (the plain question first where the port is
// StyleWriter-first, then ESC ? once that got nothing; ESC ? alone elsewhere; the plain question
// alone with the line down), and nothing follows it: not the plain question that follows an
// unanswered ESC ? elsewhere, not the cadence. A printer that answers it replaces the Something
// else record (the list's); one seen once is asked again at once (`confirm`), though the look is over.
static void test_said_other_person_look(void)
{
    // The board's own looks (boot, a stop, a flip) ask nothing, the line up or down.
    port_in_t in = base(-1);
    in.said_other = 1;
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.line_ready = 0;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);

    // StyleWriter-first, the line up: the plain question, ESC ?, then nothing for ten minutes.
    in = base(PR_KIND_IMAGEWRITER);
    in.said_other = 1;
    in.sw_listed = 1;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 1);
    CHECK_EQ_INT(in.look_person, 1);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look + in.look_person + in.event_plain, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    tally_t t = run(in, in.now_ms, in.now_ms + 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_NONE], 1200);
    // Not StyleWriter-first: ESC ? alone.
    in = base(-1);
    in.said_other = 1;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // The line down: the plain question alone.
    in = base(-1);
    in.said_other = 1;
    in.line_ready = 0;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // A StyleWriter answers it once (no record yet): the look is over, the board sets
    // `confirm`, and the repeat goes; answered again, nothing more.
    in = base(-1);
    in.said_other = 1;
    in.line_ready = 0;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_STYLEWRITER, 0);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_STYLEWRITER);
    in.confirm = PA_PLAIN;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.confirm, PA_NONE);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // A StyleWriter record that answers (the person set Something else while it did): the
    // person's look asks its status, once.
    in = base(PR_KIND_STYLEWRITER);
    in.said_other = 1;
    in.answering = 1;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    port_after(&in, PA_STATUS, PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in = at(in, 600 * SEC, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
}

// W-1116: an act the board wants repeated at once (`confirm`: a seen call answered
// PR_SEEN_AGAIN, one sighting of a model with no record; the same answer twice makes the
// record) goes first, whatever else is due or pending, and once: port_after clears it before
// anything else, and the board sets it again only on another first sighting.
static void test_confirm_repeats(void)
{
    static const port_act_t acts[] = { PA_ESCQ, PA_PLAIN, PA_STATUS, PA_KICK, PA_FULL };
    for (int k = 0; k < N_KINDS; k++)
    for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++) {
        // Nothing else due: the act, and port_after clears it.
        port_in_t in = base(KINDS[k]);
        in.answering = 1;
        in.confirm = acts[i];
        CHECK_EQ_INT(port_policy(&in), acts[i]);
        port_after(&in, acts[i], PR_HEARD_NONE, 0);
        CHECK_EQ_INT(in.confirm, PA_NONE);
        // Everything else pending: still the act.
        in = base(KINDS[k]);
        in.said_other = 1;
        in.line_up_edge = 1;
        in.look = in.look_person = 1;
        in.want_kick = 1;
        in.plain_asked = in.plain_unanswered = 1;
        in.full_due = in.sw1_listed = 1;
        in = at(in, 600 * SEC, 600 * SEC);
        in.confirm = acts[i];
        CHECK_EQ_INT(port_policy(&in), acts[i]);
    }
    // As the board runs it: ESC ? at a line-up names a model with no record (not a record's:
    // mine 0); the board sets confirm after port_after; the next call repeats ESC ? with no
    // event pending; the answer again makes the record, now current and answering, and
    // nothing follows.
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_IMAGEWRITER, 0);
    CHECK_EQ_INT(in.answering, 0);
    CHECK_EQ_INT(in.line_up_edge, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    in.confirm = PA_ESCQ;
    in.now_ms += 500;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_IMAGEWRITER, 1);
    CHECK_EQ_INT(in.confirm, PA_NONE);
    CHECK_EQ_INT(in.answering, 1);
    in.now_ms += 500;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // A StyleWriter that answers, whose status check's `?` named another model: the status
    // again at once, not in 10 s.
    in = base(PR_KIND_STYLEWRITER);
    in.answering = 1;
    in.confirm = PA_STATUS;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    port_after(&in, PA_STATUS, PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.confirm, PA_NONE);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
}

static void test_stylewriter_answering(void)
{
    port_in_t in = base(PR_KIND_STYLEWRITER);
    in.answering = 1;
    in.line_ready = 0;           // it may never raise HSKo

    // Not due: nothing, however much else would ask.
    in = at(in, 600 * SEC, 9 * SEC + 999);
    in.line_up_edge = 1;
    in.want_kick = 1;
    in.plain_unanswered = 1;
    in.plain_asked = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // Due at 10 s, not a millisecond before.
    in = at(in, 600 * SEC, 10 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    // A look asks at once.
    in = at(in, 600 * SEC, 1);
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);

    // Five minutes at the board's pace, 500 ms a call: a status every 10 s, nothing else,
    // with the plain question long overdue throughout and the printer's line up.
    port_in_t q = base(PR_KIND_STYLEWRITER);
    q.answering = 1;
    q.line_ready = 1;
    q.want_kick = 1;
    q.plain_unanswered = 1;
    q.plain_asked = 1;
    q = at(q, 0, 0);
    q.last_plain_ms = 0;
    tally_t t = run(q, 1000 * SEC, 1300 * SEC);
    CHECK_EQ_INT(t.counts[PA_STATUS], 29);       // 1010 s, 1020 s ... 1290 s
    CHECK_EQ_INT(t.counts[PA_NONE], 600 - 29);
    CHECK_EQ_INT(t.counts[PA_ESCQ] + t.counts[PA_PLAIN] + t.counts[PA_KICK], 0);
    CHECK_EQ_INT(t.at[PA_STATUS][1] - t.at[PA_STATUS][0], 10 * SEC);
}

static void test_stylewriter_silent(void)
{
    port_in_t in = base(PR_KIND_STYLEWRITER);   // answering 0: it stopped
    // The plain question every 5 s, and no status.
    in = at(in, 4 * SEC + 999, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in = at(in, 5 * SEC, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_in_t q = base(PR_KIND_STYLEWRITER);
    q.last_plain_ms = 1000 * SEC;
    tally_t t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 60);
    CHECK_EQ_INT(t.at[PA_PLAIN][0], 1005 * SEC);
    CHECK_EQ_INT(t.at[PA_PLAIN][4] - t.at[PA_PLAIN][3], 5 * SEC);
    CHECK_EQ_INT(t.counts[PA_STATUS] + t.counts[PA_ESCQ] + t.counts[PA_KICK], 0);

    // At a line-up the plain question first, so a StyleWriter switched on is never sent
    // ESC ?, whatever answered on the port before; ESC ? only once that got nothing, so an
    // ImageWriter swapped in is noticed.
    for (int heard = PR_HEARD_NONE; heard <= PR_HEARD_STYLEWRITER; heard++) {
        in = base(PR_KIND_STYLEWRITER);
        in.heard = heard;
        in.line_up_edge = 1;
        in = at(in, 1 * SEC, 600 * SEC);
        CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
        port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
        CHECK_EQ_INT(in.event_plain, 1);
        CHECK_EQ_INT(in.line_up_edge, 1);           // still to be answered
        CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
        port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
        CHECK_EQ_INT(in.line_up_edge + in.event_plain, 0);
        // the plain question was this line-up's: not again at once, but on the cadence
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
        // A look with the line up, the same.
        in = base(PR_KIND_STYLEWRITER);
        in.heard = heard;
        in.look = 1;
        CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
        port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
        CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
        // And when the StyleWriter answers it, the line-up is over: no ESC ?.
        in = base(PR_KIND_STYLEWRITER);
        in.heard = heard;
        in.line_up_edge = 1;
        CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
        port_after(&in, PA_PLAIN, PR_HEARD_STYLEWRITER, 1);
        CHECK_EQ_INT(in.line_up_edge + in.event_plain + in.look, 0);
        CHECK_EQ_INT(in.answering, 1);
        CHECK_EQ_INT(port_policy(&in), PA_NONE);   // status in 10 s
    }

    // Never the kick, however long its line has been down and its plain question dead.
    in = base(PR_KIND_STYLEWRITER);
    in.want_kick = 1;
    in.plain_unanswered = 1;
    in.plain_asked = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in = at(in, 5 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
}

static void test_imagewriter_answering(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.answering = 1;
    // ESC ? at line-up only.
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.line_up_edge = 0;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // Never the plain question, however old; ten minutes of nothing.
    in = at(in, 600 * SEC, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    tally_t t = run(in, 1000 * SEC, 1600 * SEC);
    CHECK_EQ_INT(t.counts[PA_NONE], 1200);
    // A look with the line up asks ESC ?; with the line down it asks nothing.
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.line_ready = 0;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
}

static void test_imagewriter_silent(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in = at(in, 4 * SEC + 999, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in = at(in, 5 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_in_t q = base(PR_KIND_IMAGEWRITER);
    tally_t t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 60);
    // ESC ? at line-up first, then the plain one at once (port_after makes it due).
    in = base(PR_KIND_IMAGEWRITER);
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // An ESC ? that names it: answering, nothing more.
    in = base(PR_KIND_IMAGEWRITER);
    in.line_up_edge = 1;
    port_after(&in, port_policy(&in), PR_HEARD_IMAGEWRITER, 1);
    CHECK_EQ_INT(in.answering, 1);
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
}

// W-1116: a port whose printer is not answering is asked the plain question every 5 s to the
// tick, whatever the list holds and whatever answered last, the line up or down, and nothing
// else: so a StyleWriter plugged in is found within 5 s by someone who never added it.
static void test_quiet_cadence(void)
{
    static const int kinds[] = { -1, PR_KIND_IMAGEWRITER, PR_KIND_STYLEWRITER };
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
    for (int line = 0; line <= 1; line++)
    for (int heard = PR_HEARD_NONE; heard <= PR_HEARD_STYLEWRITER; heard++) {
        port_in_t in = base(kinds[k]);
        in.line_ready = line;
        in.heard = heard;
        tally_t t = run(in, 1000 * SEC, 1600 * SEC + 1);
        CHECK_EQ_INT(t.counts[PA_PLAIN], 120);
        CHECK_EQ_INT(t.at[PA_PLAIN][0], 1005 * SEC);
        for (int i = 1; i < 64; i++) CHECK_EQ_INT(t.at[PA_PLAIN][i] - t.at[PA_PLAIN][i - 1], 5 * SEC);
        CHECK_EQ_INT(t.counts[PA_NONE], 1201 - 120);
        CHECK_EQ_INT(t.counts[PA_ESCQ] + t.counts[PA_STATUS] + t.counts[PA_KICK] + t.counts[PA_FULL], 0);
    }
    // A stamp between ticks: the question goes at the first tick 5 s or more after it, and
    // every 5 s from there.
    port_in_t in = base(-1);
    in.last_plain_ms = in.now_ms - 250;
    tally_t t = run(in, in.now_ms, in.now_ms + 60 * SEC + 1);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 12);
    CHECK_EQ_INT(t.at[PA_PLAIN][0], in.now_ms + 5 * SEC);
    for (int i = 1; i < 12; i++) CHECK_EQ_INT(t.at[PA_PLAIN][i] - t.at[PA_PLAIN][i - 1], 5 * SEC);
}

// The original ImageWriter gives no self-ID: with its record listed, an ESC ? that gets nothing
// with the line up is it answering (the board makes its record current), so it is not asked the
// plain question every 5 s for as long as it is on. Its record makes the port StyleWriter-first,
// so a line-up asks the plain question first, and a StyleWriter in its place answers before it
// hears ESC ?.
static void test_original_imagewriter(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.iw1_listed = 1;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 1);
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    tally_t t = run(in, in.now_ms, in.now_ms + 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_NONE], 1200);
    // The same whatever record is current: a StyleWriter record that stopped answering, or
    // none (W-1116: the record need not be the current one, only listed).
    in = base(PR_KIND_STYLEWRITER);
    in.iw1_listed = 1;
    in.heard = PR_HEARD_STYLEWRITER;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 1);
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    in = base(-1);
    in.iw1_listed = 1;
    in.look = 1;
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    // With its line down, ESC ? getting nothing is not an answer.
    in = base(PR_KIND_IMAGEWRITER);
    in.iw1_listed = 1;
    in.line_ready = 0;
    in.event_plain = 1;
    in.line_up_edge = 1;
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_NONE);
    // With no original ImageWriter record listed, silence is no answer.
    in = base(PR_KIND_IMAGEWRITER);
    in.line_up_edge = 1;
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_NONE);
}

static void test_no_record(void)
{
    // A look with the line up asks ESC ?; port_after clears the look, and when that got no
    // answer makes a plain question due, which the next call asks.
    port_in_t in = base(-1);
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    // With the line down a look asks the plain question straight away.
    in = base(-1);
    in.look = 1;
    in.line_ready = 0;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // And every 5 s with nothing else going on, line up or not.
    port_in_t q = base(-1);
    tally_t t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 60);
    q.line_ready = 0;
    t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 60);
    // "Answering" means nothing with no record.
    in = base(-1);
    in.answering = 1;
    in = at(in, 5 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    // A line-up edge asks ESC ? first: an empty port's order is the ImageWriter's.
    in = base(-1);
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
}

static void test_kick(void)
{
    // The kick is the last resort: wanted, the plain question since the line went down dead,
    // nothing else to ask.
    for (int k = 0; k < N_KINDS; k++) {
        port_in_t in = base(KINDS[k]);
        in.want_kick = 1;
        in.plain_asked = 1;
        in.plain_unanswered = 1;
        int sw = KINDS[k] == PR_KIND_STYLEWRITER;
        CHECK_EQ_INT(port_policy(&in), sw ? PA_NONE : PA_KICK);
        in.want_kick = 0;
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
        // No plain question since the line went down, so one is asked at once, however
        // recently the last one was; a StyleWriter record never gets that or the kick.
        in.want_kick = 1;
        in.plain_asked = 0;
        in.plain_unanswered = 0;
        in = at(in, 1 * SEC, 0);
        CHECK_EQ_INT(port_policy(&in), sw ? PA_NONE : PA_PLAIN);
        // One that a printer answered is no reason to kick.
        in.plain_asked = 1;
        in.plain_unanswered = 0;
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
    }
    // An ImageWriter stuck busy at power-up, as the board runs it: its line is down, the
    // plain question comes due at 5 s and gets nothing, and the kick follows on the next
    // call and every call after, for printer_state to pace.
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in.plain_asked = 1;
    in.last_plain_ms = in.now_ms - 5 * SEC;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_KICK);
    // ESC ? and the plain question both outrank it.
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.line_up_edge = 0;
    in.last_plain_ms = in.now_ms - 5 * SEC;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    // port_tick: an ImageWriter that answered and has now been down the kick delay is not
    // answering; a StyleWriter record is not touched.
    in = base(PR_KIND_IMAGEWRITER);
    in.answering = 1;
    port_tick(&in);
    CHECK_EQ_INT(in.answering, 1);
    in.want_kick = 1;
    port_tick(&in);
    CHECK_EQ_INT(in.answering, 0);
    in = base(PR_KIND_STYLEWRITER);
    in.answering = 1;
    in.want_kick = 1;
    port_tick(&in);
    CHECK_EQ_INT(in.answering, 1);
    // port_line_down: a new busy spell.
    in.plain_asked = in.plain_unanswered = 1;
    port_line_down(&in);
    CHECK_EQ_INT(in.plain_asked + in.plain_unanswered, 0);
}

// A busy ImageWriter is kicked about as soon as it is wanted, not a plain-question cadence
// later. port_line_down clears plain_asked when the line goes down, and the busy spell gets
// one fresh plain question, then the kick; one that a StyleWriter answers gets neither.
static void test_kick_prompt(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in = at(in, 1 * SEC, 0);                       // asked a second ago, for another spell
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);   // it got nothing
    CHECK_EQ_INT(port_policy(&in), PA_KICK);

    // The same with no record (a port nothing has been found on, left busy at power-up).
    in = base(-1);
    in.line_ready = 0;
    in.want_kick = 1;
    in = at(in, 1 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_KICK);

    // A StyleWriter record never gets either on this path, answering or not.
    in = base(PR_KIND_STYLEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in = at(in, 1 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.plain_asked = in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.answering = 1;
    in = at(in, 1 * SEC, 1 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.plain_unanswered = 0;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);

    // An ImageWriter that answers is not asked the plain question by a wanted kick.
    in = base(PR_KIND_IMAGEWRITER);
    in.answering = 1;
    in.want_kick = 1;
    in = at(in, 1 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // (If its flag is stale and the plain question had got nothing, it is kicked.)
    in.plain_asked = in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_KICK);

    // The person said "Something else": nothing, wanted or not.
    in = base(PR_KIND_IMAGEWRITER);
    in.said_other = 1;
    in.want_kick = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.plain_asked = in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);

    // As the board runs it: the line drops at 100 s with the printer busy (its last plain
    // question was 2.5 s before, for an earlier spell), and want_kick comes true at 105 s.
    // Calls every 500 ms: the cadence's plain question at 102.5 s is the busy spell's, so the
    // kick goes the moment it is wanted, then kicks for printer_state to pace.
    in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.last_plain_ms = 97 * SEC + 500;
    in.plain_asked = 1;
    port_line_down(&in);
    int plains = 0, kicks = 0;
    uint32_t first_plain = 0, first_kick = 0;
    for (uint32_t ms = 100 * SEC; ms < 130 * SEC; ms += 500) {
        in.now_ms = ms;
        in.want_kick = ms >= 105 * SEC;
        port_tick(&in);
        port_act_t a = port_policy(&in);
        if (a == PA_PLAIN && !plains++) first_plain = ms;
        if (a == PA_KICK && !kicks++) first_kick = ms;
        port_after(&in, a, PR_HEARD_NONE, 0);      // a busy ImageWriter says nothing
    }
    CHECK_EQ_INT(first_plain, 102 * SEC + 500);
    CHECK_EQ_INT(first_kick, 105 * SEC);
    CHECK(plains >= 6);                            // the cadence goes on, 5 s apart
    CHECK(kicks >= 1);

    // An ImageWriter record with a StyleWriter on its port, which does not raise the line, so
    // the kick is wanted for as long as it is there: the plain question every 5 s, which the
    // StyleWriter answers, so no CTRL-X to it (W-1116: the board makes its record current
    // once it has answered twice; here the view stays the ImageWriter record's).
    in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.last_plain_ms = 95 * SEC;
    plains = kicks = 0;
    for (uint32_t ms = 100 * SEC; ms < 700 * SEC; ms += 500) {
        in.now_ms = ms;
        in.want_kick = ms >= 105 * SEC;
        port_tick(&in);
        port_act_t a = port_policy(&in);
        plains += a == PA_PLAIN;
        kicks += a == PA_KICK;
        port_after(&in, a, a == PA_PLAIN ? PR_HEARD_STYLEWRITER : PR_HEARD_NONE, 0);
    }
    CHECK_EQ_INT(plains, 120);                     // 100 s, 105 s ... 695 s
    CHECK_EQ_INT(kicks, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_STYLEWRITER);
    // A look asks it again (the person asked), once.
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_STYLEWRITER, 0);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // It is switched off and the person looks: nothing answers, so the kick may follow (an
    // ImageWriter back in its place and stuck busy is freed).
    in.look = 1;
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    in.want_kick = 1;
    CHECK_EQ_INT(port_policy(&in), PA_KICK);
}

// Times past the 32-bit millisecond clock's wrap are not a reason to ask or not to ask.
static void test_wrap(void)
{
    port_in_t in = base(-1);
    in.last_plain_ms = 0xFFFFFF00u;
    in.now_ms = 0xFFFFFF00u + 5 * SEC - 1;      // wraps past zero
    CHECK(in.now_ms < in.last_plain_ms);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.now_ms++;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);

    in = base(PR_KIND_STYLEWRITER);
    in.answering = 1;
    in.last_status_ms = 0xFFFFFFF0u;
    in.now_ms = 0xFFFFFFF0u + 10 * SEC - 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.now_ms++;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);

    // A StyleWriter that answered for a month and stops: the plain stamp is older than half
    // the clock's range and the question is still due (unsigned, not signed, differences).
    in = base(-1);
    in.now_ms = 3000000000u;
    in.last_plain_ms = 100000u;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);

    // The stamp that makes a plain question due works across the wrap too.
    for (uint32_t now = 0xFFFFF000u; now != 0x2000u; now += 0x1000u) {
        in = base(-1);
        in.now_ms = now;
        in.last_plain_ms = port_plain_due(now);
        CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    }
}

// The plain question asked for a line-up or a look goes with that event: once the event is
// over another way, the next line-up on a StyleWriter-first port takes the plain question
// first again, not ESC ?.
static void test_stale_event(void)
{
    // A look with the line up; the plain question gets nothing; the line drops before the next
    // call, so the look ends with a plain question with the line down.
    port_in_t in = base(PR_KIND_STYLEWRITER);
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.event_plain, 1);
    in.line_ready = 0;
    in.now_ms += 500;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(in.event_plain, 0);
    in.now_ms += 30 * SEC;
    in.line_ready = 1;
    in.line_up_edge = 1;                        // later: switched on, its line up
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);

    // A line-up and a look; the plain question gets nothing; before the next call the record's
    // printer answers something else (a job, the person's pick), and the look's status ends it.
    in = base(PR_KIND_STYLEWRITER);
    in.line_up_edge = 1;
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.event_plain, 1);
    in.answering = 1;
    in.now_ms += 500;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    port_after(&in, PA_STATUS, PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.event_plain, 0);
    in.now_ms += 30 * SEC;
    in.answering = 0;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
}

// Review Focus 1: a StyleWriter that raises its handshake line, plugged in after an
// ImageWriter. The ImageWriter's record is current (it answered last) and the StyleWriter's is
// listed: the line-up edge asks the plain question before any ESC ?, the StyleWriter answers it
// and is never sent ESC ? (which would leave it deaf) nor the kick. With no StyleWriter record
// listed and an ImageWriter answered last, it is ESC ? first, as built (the design's cost).
static void test_swap_sw_raises_line(void)
{
    for (int answering = 0; answering <= 1; answering++) {   // the ImageWriter's flag: fresh, or stale from a drop under the kick delay
        port_in_t in = base(PR_KIND_IMAGEWRITER);
        in.heard = PR_HEARD_IMAGEWRITER;
        in.sw_listed = 1;
        in.answering = answering;
        in.line_up_edge = 1;
        CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
        port_after(&in, PA_PLAIN, PR_HEARD_STYLEWRITER, 1);
        CHECK_EQ_INT(in.line_up_edge, 0);
        CHECK_EQ_INT(in.heard, PR_HEARD_STYLEWRITER);
        CHECK_EQ_INT(in.answering, 1);
        in.kind = PR_KIND_STYLEWRITER;                          // the board: its record is current now
        CHECK_EQ_INT(port_policy(&in), PA_NONE);                // its status in 10 s
        tally_t t = run(in, in.now_ms, in.now_ms + 60 * SEC);
        CHECK_EQ_INT(t.counts[PA_ESCQ] + t.counts[PA_PLAIN] + t.counts[PA_KICK], 0);
        CHECK_EQ_INT(t.counts[PA_STATUS], 5);
    }
    // The same with an original ImageWriter record listed, or a StyleWriter answered last.
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.heard = PR_HEARD_IMAGEWRITER;
    in.iw1_listed = 1;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    in = base(PR_KIND_IMAGEWRITER);
    in.heard = PR_HEARD_STYLEWRITER;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    // No StyleWriter record, an ImageWriter answered last: ESC ? first.
    in = base(PR_KIND_IMAGEWRITER);
    in.heard = PR_HEARD_IMAGEWRITER;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
}

// Review Focus 4: a line that blips. The board raises a line-up edge only after the line was
// down a second or more (W-1064), so a drop under 1 s (power-on, a quick deselect, the
// printer's own busy blips) is no event here: no ESC ?, no plain question, and the ImageWriter
// stays answering, though the board calls port_line_down for every drop. A drop of a second or
// more is an edge, and one ESC ?.
static void test_line_blips(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.answering = 1;
    in.heard = PR_HEARD_IMAGEWRITER;
    uint32_t t0 = in.now_ms, low_at = 0;
    int line = 1, escq = 0, plains = 0, kicks = 0, answering_all_along = 1;
    for (uint32_t ms = t0; ms < t0 + 120 * SEC; ms += 500) {
        // one tick down at 10 s, 30 s and 50 s; 1.5 s down from 90 s
        int drop = ms == t0 + 10 * SEC || ms == t0 + 30 * SEC || ms == t0 + 50 * SEC ||
                   (ms >= t0 + 90 * SEC && ms < t0 + 91 * SEC + 500);
        int line_now = !drop;
        if (line && !line_now) { low_at = ms; port_line_down(&in); }
        if (!line && line_now && ms - low_at >= 1 * SEC) in.line_up_edge = 1;
        line = line_now;
        in.now_ms = ms;
        in.line_ready = line;
        in.want_kick = 0;                        // printer_state wants one only after 5 s down
        port_tick(&in);
        port_act_t a = port_policy(&in);
        escq += a == PA_ESCQ;
        plains += a == PA_PLAIN;
        kicks += a == PA_KICK;
        int heard = a == PA_ESCQ && line ? PR_HEARD_IMAGEWRITER : PR_HEARD_NONE;
        port_after(&in, a, heard, heard != PR_HEARD_NONE);
        if (ms < t0 + 90 * SEC && !in.answering) answering_all_along = 0;
    }
    CHECK_EQ_INT(answering_all_along, 1);
    CHECK_EQ_INT(escq, 1);                       // the 1.5 s drop's edge, at 91.5 s
    CHECK_EQ_INT(plains + kicks, 0);
    CHECK_EQ_INT(in.answering, 1);
}

// W-1116: an ImageWriter record whose port a StyleWriter answered (one with no record yet, its
// first sighting, or after the person's model change) is asked as any port that is not
// answering: the plain question every 5 s, which the StyleWriter answers, so no kick while it
// does; switched off, the plain question gets nothing and the kick follows, paced by
// printer_state; switched on again, its next plain question answers and the kicks stop. The
// StyleWriter is switched off at 2 min, the person looks at 3 min, it is switched on again at
// 4 min, its line down throughout. (The StyleWriter branch's rule, nothing of the record's own
// and one kick a look, went with the "changed" notice it waited for.)
static void test_stylewriter_heard(void)
{
    port_in_t in;
    memset(&in, 0, sizeof in);
    in.kind = PR_KIND_IMAGEWRITER;
    in.heard = PR_HEARD_STYLEWRITER;
    in.line_ready = 0;
    uint32_t t = 1000 * SEC;
    in.now_ms = in.last_plain_ms = t;
    int sw_on = 1, kicks = 0, kicks_to_sw = 0, kicked = 0, plains_on = 0, plains_off = 0;
    uint32_t kicked_at = 0;
    for (uint32_t ms = t; ms < t + 900 * SEC; ms += 500) {
        in.now_ms = ms;
        if (ms == t + 120 * SEC) sw_on = 0;
        if (ms == t + 180 * SEC) in.look = in.look_person = 1;
        if (ms == t + 240 * SEC) sw_on = 1;
        in.want_kick = ms - t >= 5 * SEC && (!kicked || ms - kicked_at >= 60 * SEC);
        port_tick(&in);
        port_act_t a = port_policy(&in);
        int heard = PR_HEARD_NONE;
        if (a == PA_PLAIN) {
            if (sw_on) { heard = PR_HEARD_STYLEWRITER; plains_on++; }
            else plains_off++;
        }
        if (a == PA_KICK) {
            kicked = 1;
            kicked_at = ms;
            kicks++;
            if (sw_on) kicks_to_sw++;
        }
        port_after(&in, a, heard, 0);
    }
    CHECK_EQ_INT(kicks_to_sw, 0);
    CHECK_EQ_INT(kicks, 2);                      // 120.5 s and 180.5 s, while it was off
    CHECK_EQ_INT(plains_on, 155);                // 5 s ... 115 s, then 240 s ... 895 s
    CHECK_EQ_INT(plains_off, 24);                // 120 s ... 235 s, the look's among them
    CHECK_EQ_INT(in.heard, PR_HEARD_STYLEWRITER);
    // A look that a printer answered allows no kick either.
    in.look = in.look_person = 1;
    port_after(&in, port_policy(&in), PR_HEARD_STYLEWRITER, 0);
    in.want_kick = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
}

// An original StyleWriter record listed: its printer ignores the plain question until a reset
// (InkTalk's notes), so only the person's Look for Printers can find it again. The look asks the
// plain question and ESC ? first, and only when nothing answers sends the full sequence, once,
// whatever answered on the port before and whatever record is current (W-1116).
static void test_original_look(void)
{
    // Its line up: the plain question, ESC ?, then the full sequence; the original answers it.
    port_in_t in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.heard = PR_HEARD_IMAGEWRITER;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.full_due, 1);
    CHECK_EQ_INT(port_policy(&in), PA_FULL);
    port_after(&in, PA_FULL, PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.full_due, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_STYLEWRITER);
    CHECK_EQ_INT(in.answering, 1);
    // Its line down: the plain question, then the full sequence.
    in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.heard = PR_HEARD_IMAGEWRITER;
    in.line_ready = 0;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_FULL);
    // Nothing answers that either: it is not sent again, not on the cadence, not by a later
    // look of the board's own.
    port_after(&in, PA_FULL, PR_HEARD_NONE, 0);
    port_in_t q = in;
    tally_t t = run(q, in.now_ms, in.now_ms + 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_FULL], 0);
    in.look = 1;                                  // a look that is not the person's (a stop, a flip)
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    CHECK_EQ_INT(in.full_due, 0);
    // The ImageWriter is still there: ESC ? answers, and no full sequence follows.
    in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.heard = PR_HEARD_IMAGEWRITER;
    in.look = in.look_person = 1;
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_IMAGEWRITER, 0);
    CHECK_EQ_INT(in.full_due, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // An original record listed beside whatever record is current, whatever answered last:
    // the same. Never without one.
    static const struct { int kind, sw1_listed, heard; } any[] = {
        { PR_KIND_STYLEWRITER, 1, PR_HEARD_STYLEWRITER }, { PR_KIND_STYLEWRITER, 1, PR_HEARD_NONE },
        { PR_KIND_IMAGEWRITER, 1, PR_HEARD_IMAGEWRITER }, { -1, 1, PR_HEARD_NONE },
        { PR_KIND_UNKNOWN, 1, PR_HEARD_NONE },
        { PR_KIND_STYLEWRITER, 0, PR_HEARD_IMAGEWRITER }, { PR_KIND_STYLEWRITER, 0, PR_HEARD_STYLEWRITER },
        { PR_KIND_STYLEWRITER, 0, PR_HEARD_NONE }, { PR_KIND_IMAGEWRITER, 0, PR_HEARD_NONE },
        { -1, 0, PR_HEARD_NONE },
    };
    for (size_t i = 0; i < sizeof any / sizeof any[0]; i++) {
        in = base(any[i].kind);
        in.sw1_listed = any[i].sw1_listed;
        in.heard = any[i].heard;
        in.line_ready = 0;
        in.look = in.look_person = 1;
        port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
        CHECK_EQ_INT(in.full_due, any[i].sw1_listed);
        CHECK_EQ_INT(port_policy(&in), (any[i].sw1_listed ? PA_FULL : PA_NONE));
    }
    // An original that answers its status when the person looks needs nothing more; one that
    // has stopped answering gets the look's questions and then the full sequence.
    in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.answering = 1;
    in.heard = PR_HEARD_STYLEWRITER;
    in.look = in.look_person = 1;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    port_after(&in, PA_STATUS, PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.look, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.look = in.look_person = 1;
    in.line_ready = 0;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    port_after(&in, PA_STATUS, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look_person, 1);                // still the person's look
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_FULL);
    // Never while the port's printer answers: an ImageWriter record answering, with the
    // original's record listed and the sequence owed, waits; once it stops, the sequence goes.
    in = base(PR_KIND_IMAGEWRITER);
    in.sw1_listed = 1;
    in.answering = 1;
    in.full_due = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.answering = 0;
    CHECK_EQ_INT(port_policy(&in), PA_FULL);
    // And never with no original StyleWriter record listed, whatever the flags say.
    in = base(PR_KIND_STYLEWRITER);
    in.heard = PR_HEARD_IMAGEWRITER;
    in.full_due = 1;
    CHECK(port_policy(&in) != PA_FULL);
}

// At start-up with an original StyleWriter record listed, the full sequence goes once (the
// board sets full_due), so an empty port reads offline and not ready (Wells, 2026-10-27). Any
// answer to a question before it (an event's) makes it unneeded; nothing sends it again on its own.
static void test_original_boot(void)
{
    // Nothing else pending: the full sequence first, then the 5 s plain question only.
    port_in_t in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.line_ready = 0;
    in.full_due = 1;
    CHECK_EQ_INT(port_policy(&in), PA_FULL);
    port_after(&in, PA_FULL, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.full_due, 0);
    tally_t t = run(in, in.now_ms + 500, in.now_ms + 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_FULL], 0);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 119);         // 5 s after start, then every 5 s
    // It answers: its status from then on.
    in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.full_due = 1;
    port_after(&in, port_policy(&in), PR_HEARD_STYLEWRITER, 1);
    CHECK_EQ_INT(in.answering, 1);
    CHECK_EQ_INT(in.full_due, 0);
    t = run(in, in.now_ms + 500, in.now_ms + 60 * SEC);
    CHECK_EQ_INT(t.counts[PA_FULL] + t.counts[PA_PLAIN] + t.counts[PA_ESCQ], 0);
    CHECK(t.counts[PA_STATUS] >= 5);
    // Its line came up at start: the plain question and ESC ? first. An ImageWriter answers ESC ?:
    // no reset follows (it would read as a stray character there).
    in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.full_due = 1;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_IMAGEWRITER, 0);
    CHECK_EQ_INT(in.full_due, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // Nothing answers either: the full sequence follows.
    in = base(PR_KIND_STYLEWRITER);
    in.sw1_listed = 1;
    in.full_due = 1;
    in.line_up_edge = 1;
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_FULL);
    // The board's own look at start-up (the record's family is new to the port task) goes
    // first, and leaves the full sequence owed, its line up or down.
    for (int line = 0; line <= 1; line++) {
        in = base(PR_KIND_STYLEWRITER);
        in.sw1_listed = 1;
        in.full_due = 1;
        in.look = 1;
        in.line_ready = line;
        for (int i = 0; i < 2 && port_policy(&in) != PA_FULL; i++)
            port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
        CHECK_EQ_INT(in.look, 0);
        CHECK_EQ_INT(port_policy(&in), PA_FULL);
    }
}

// A StyleWriter record that was answering and stops gets one look at once: the plain question,
// then ESC ? if that gets nothing. So an ImageWriter put in its place with its line already up
// (behind an A/B switch, its line-up passed while the StyleWriter still answered) is found within
// one status interval, and the port's last answer is the ImageWriter's.
static void test_stop_looks(void)
{
    port_in_t in = base(PR_KIND_STYLEWRITER);
    in.answering = 1;
    in.heard = PR_HEARD_STYLEWRITER;
    uint32_t t0 = in.now_ms, swap = t0 + 23 * SEC, found = 0;
    int iw_there = 0, escq = 0, plains = 0, sw_heard_escq = 0;
    for (uint32_t ms = t0; ms < t0 + 400 * SEC; ms += 500) {
        in.now_ms = ms;
        if (ms == swap) iw_there = 1;             // its line already up; the edge went by
        port_tick(&in);
        port_act_t a = port_policy(&in);
        int heard = PR_HEARD_NONE, mine = 0;
        if (a == PA_STATUS && !iw_there) heard = PR_HEARD_STYLEWRITER, mine = 1;
        if (a == PA_PLAIN && !iw_there) heard = PR_HEARD_STYLEWRITER, mine = 1;
        if (a == PA_ESCQ) {
            escq++;
            if (!iw_there) sw_heard_escq++;
            else heard = PR_HEARD_IMAGEWRITER;
        }
        plains += a == PA_PLAIN;
        port_after(&in, a, heard, mine);
        if (!found && in.heard == PR_HEARD_IMAGEWRITER) found = ms;
    }
    CHECK(found > swap && found - swap <= 11 * SEC);
    // The port last answered as an ImageWriter: the StyleWriter record reads offline and its
    // jobs are answered busy (pr_port_absent, W-1116); the board makes the ImageWriter's
    // record current, which this frozen view does not.
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    CHECK_EQ_INT(escq, 1);                          // once per stop, not again
    CHECK_EQ_INT(sw_heard_escq, 0);
    CHECK(plains >= 60 && plains <= 1 + 400 / 5);   // the stop's, then the 5 s cadence

    // Switched off and left off: one plain question at once, then the cadence; no more looks.
    in = base(PR_KIND_STYLEWRITER);
    in.answering = 1;
    in.line_ready = 0;
    in.last_status_ms = in.now_ms - 10 * SEC;
    CHECK_EQ_INT(port_policy(&in), PA_STATUS);
    port_after(&in, PA_STATUS, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 1);
    CHECK_EQ_INT(in.look_person, 0);               // the board's look, not the person's
    in.now_ms += 500;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 0);
    tally_t t = run(in, in.now_ms + 500, in.now_ms + 300 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 59);          // 5, 10 ... 295 s after
    CHECK_EQ_INT(t.counts[PA_ESCQ] + t.counts[PA_STATUS], 0);
    // A status that a StyleWriter record that was not answering asked gets nothing: no look.
    in = base(PR_KIND_STYLEWRITER);
    port_after(&in, PA_STATUS, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.look, 0);
}

// ---- ten minutes of the port task against a scripted printer

// What is on the port.
enum { NOTHING, IMAGEWRITER, ORIGINAL_IW, SW_MINE, SW_OTHER, N_ON_PORT };
static const char *const ON_PORT[] = { "nothing", "an ImageWriter II", "an original ImageWriter",
                                       "the record's StyleWriter", "another StyleWriter" };

typedef struct {
    int on, line;                 // what is there, and its HSKo
    int off;                      // switched off: it answers nothing and its line is down
    int deaf;                     // a StyleWriter that heard ESC ? and stopped listening
    // what it heard
    int plains, escq, escq_sw_awake, kicks_sw, status, looks;
    uint32_t plain_at[64];
    uint32_t min_gap;             // the least time between two plain questions not asked for a look or a line-up
} world_t;

static int is_sw(const world_t *w) { return !w->off && (w->on == SW_MINE || w->on == SW_OTHER); }

// Who answers `a`: the family, and *mine whether it is the current record's own printer.
static int world_answer(world_t *w, const port_in_t *in, port_act_t a, int *mine)
{
    *mine = 0;
    switch (a) {
    case PA_ESCQ:
        w->escq++;
        if (is_sw(w)) {
            if (!w->deaf) w->escq_sw_awake++;
            w->deaf = 1;                                      // swfake: E0 is no letter it knows
            return PR_HEARD_NONE;
        }
        if (w->on == IMAGEWRITER && w->line && !w->off) {
            *mine = in->kind == PR_KIND_IMAGEWRITER;
            return PR_HEARD_IMAGEWRITER;
        }
        return PR_HEARD_NONE;                                 // the original says nothing
    case PA_PLAIN:
        if (!is_sw(w) || w->deaf) return PR_HEARD_NONE;
        *mine = in->kind == PR_KIND_STYLEWRITER && w->on == SW_MINE;
        return PR_HEARD_STYLEWRITER;
    case PA_STATUS:
        w->status++;
        return is_sw(w) && !w->deaf ? PR_HEARD_STYLEWRITER : PR_HEARD_NONE;
    case PA_KICK:
        if (is_sw(w)) w->kicks_sw++;
        return PR_HEARD_NONE;
    case PA_FULL:                                             // the reset clears a deaf one too
        if (!is_sw(w)) return PR_HEARD_NONE;
        w->deaf = 0;
        *mine = in->kind == PR_KIND_STYLEWRITER && w->on == SW_MINE;
        return PR_HEARD_STYLEWRITER;
    default:
        return PR_HEARD_NONE;
    }
}

// The records on the port as the policy sees them: the current one's kind, and what the list
// holds. Frozen for a run (the board would make the record that answers current; the policy
// must behave under any view).
typedef struct { int kind, iw1_listed, sw_listed; const char *name; } rec_t;

// The board's loop: every 500 ms, or at once after ESC ? (`again`), with printer_state's kick
// timing (down 5 s, then at most every 60 s) and the first plain question 5 s after start.
// With `events`, the printer is switched off at 2 min, the person looks at 3 min, it is switched
// on again at 4 min (its line up again if it was, a line-up), and the person looks at 6 min.
static world_t ten_minutes(const rec_t *rec, int on, int line, int boot_look, int events)
{
    world_t w;
    memset(&w, 0, sizeof w);
    w.on = on;
    w.line = line;
    w.looks = boot_look;
    w.min_gap = 0xFFFFFFFFu;
    port_in_t in;
    memset(&in, 0, sizeof in);
    in.kind = rec->kind;
    in.iw1_listed = rec->iw1_listed;
    in.sw_listed = rec->sw_listed;
    uint32_t t0 = 5000u * SEC, end = t0 + 600 * SEC;
    in.last_plain_ms = t0;
    in.look = boot_look;
    in.line_up_edge = line;                                   // first seen, 2 s in
    uint32_t down_since = t0, kicked_at = 0, last_plain = 0;
    int kicked = 0, have_plain = 0, again = 0;
    for (uint32_t ms = t0; ms < end; ms += again ? 0 : 500) {
        if (events && !again) {
            uint32_t at_s = (ms - t0) / SEC;
            if (ms - t0 == at_s * SEC && (at_s == 180 || at_s == 360)) {
                in.look = in.look_person = 1;
                w.looks++;
            }
            if (ms - t0 == 120 * SEC) {
                w.off = 1;
                w.deaf = 0;                                   // switched off: whatever it heard is gone
                if (w.line) { port_line_down(&in); down_since = ms; }
            }
            if (ms - t0 == 240 * SEC) {
                w.off = 0;
                if (w.line) { in.line_up_edge = 1; kicked = 0; }     // printer_state: up resets the kick
            }
        }
        again = 0;
        int asked_for_event = in.look || in.line_up_edge;
        in.now_ms = ms;
        int line_now = w.line && !w.off;
        in.line_ready = line_now;
        in.want_kick = !line_now && ms - down_since >= 5 * SEC &&
                       (!kicked || ms - kicked_at >= 60 * SEC);
        port_tick(&in);
        port_act_t a = port_policy(&in);
        if (a == PA_PLAIN) {
            if (w.plains < 64) w.plain_at[w.plains] = ms;
            w.plains++;
            if (have_plain && !asked_for_event && ms - last_plain < w.min_gap) w.min_gap = ms - last_plain;
            have_plain = 1;
            last_plain = ms;
        }
        if (a == PA_KICK) {
            kicked = 1;
            kicked_at = ms;
        }
        int mine;
        int heard = world_answer(&w, &in, a, &mine);
        port_after(&in, a, heard, mine);
        if (a == PA_ESCQ) again = 1;
    }
    return w;
}

// Every record against every printer, its line up or down, with a look at boot or not:
// the plain question every 5 s of its own accord while the port's printer is not answering,
// and never more often; no StyleWriter that would answer it hears ESC ? first where the port
// is StyleWriter-first; no StyleWriter ever hears CTRL-X; a StyleWriter that answers as the
// record's is asked its status.
static void test_ten_minutes(void)
{
    static const rec_t recs[] = {
        { -1, 0, 0, "no record" }, { PR_KIND_UNKNOWN, 0, 0, "an unknown record" },
        { PR_KIND_IMAGEWRITER, 0, 0, "an ImageWriter II record" },
        { PR_KIND_IMAGEWRITER, 1, 0, "an original ImageWriter record" },
        { PR_KIND_STYLEWRITER, 0, 1, "a StyleWriter record" },
        { PR_KIND_IMAGEWRITER, 0, 1, "an ImageWriter II record beside a StyleWriter record" },
    };
    for (size_t r = 0; r < sizeof recs / sizeof recs[0]; r++)
    for (int on = 0; on < N_ON_PORT; on++)
    for (int line = 0; line <= 1; line++)
    for (int look = 0; look <= 1; look++)
    for (int ev = 0; ev <= 1; ev++) {
        world_t w = ten_minutes(&recs[r], on, line, look, ev);
        int sw = recs[r].kind == PR_KIND_STYLEWRITER, iw = recs[r].kind == PR_KIND_IMAGEWRITER;
        int iw1 = recs[r].iw1_listed, sw_first = recs[r].sw_listed || iw1;
        int fail = 0;
        // at most every 5 s, after whatever a look or a line-up asked
        if (w.min_gap < 5 * SEC) fail = 1;
        if (w.plains > 125) fail = 1;
        // CTRL-X never reaches a StyleWriter.
        if (w.kicks_sw) fail = 1;
        // A StyleWriter-first port never sends ESC ? to a StyleWriter that would have answered.
        if (sw_first && w.escq_sw_awake) fail = 1;
        // The record's own StyleWriter, answering: one plain question, then its status.
        if (!ev && sw && on == SW_MINE && (w.plains != 1 || w.status < 50)) fail = 1;
        if (ev && sw && on == SW_MINE && w.status < 30) fail = 1;
        // Left alone: a port whose printer answers (the record's StyleWriter; an ImageWriter II
        // answering ESC ? at its line-up; an original ImageWriter record's line up with nothing
        // said) is asked the plain question at its line-up at most, where the port is
        // StyleWriter-first; any other is asked it every 5 s, 119 or 120 times.
        int quiet = (sw && on == SW_MINE) ||
                    (iw && line && (on == IMAGEWRITER || (iw1 && (on == ORIGINAL_IW || on == NOTHING))));
        if (!ev && quiet && w.plains > (sw_first ? 1 : 0)) fail = 1;
        if (!ev && !quiet && (w.plains < 119 || w.plains > 120)) fail = 1;
        CHECK(!fail);
        if (fail)
            fprintf(stderr, "  %s, %s on the port, line %s, %s%s: %d plain (closest %u ms apart), "
                    "%d ESC ? (%d to a listening StyleWriter), %d kicks to a StyleWriter, "
                    "%d looks, %d status\n",
                    recs[r].name, ON_PORT[on], line ? "up" : "down", look ? "a look at boot" : "no look",
                    ev ? ", switched off, looked, on, looked" : "", w.plains, (unsigned)w.min_gap,
                    w.escq, w.escq_sw_awake, w.kicks_sw, w.looks, w.status);
    }
}

// ---- a script of swaps, with the list kept as the board keeps it

// W-1116: the port's records as printers_core keeps them, one per model. A model that answers
// twice in a row gets a record (the first sighting is PR_SEEN_AGAIN, and the board repeats the
// question at once, `confirm`); any other answer in between starts over; the one that answered
// last is the current record, whose kind the policy is given, and `mine` is an answer that
// names a record. An original ImageWriter's record is only ever the person's (it answers
// nothing), so a script that needs it starts with it. A StyleWriter answering a status check
// is named by the check's `?` (the board's, W-1116). `on` changes at the script's times: the
// cable out and in again, the line as the printer plugged in has it, and between two printers
// that raise the line the drop is a second or more (the board's edge rule, W-1064).
typedef struct { uint32_t at_s; int on; } swap_t;

typedef struct {
    int listed[N_ON_PORT];        // a record for the model
    int current;                  // the record on the port, NOTHING for none
    int pending;                  // a model seen once, NOTHING for none
    int sw_line;                  // the StyleWriters raise HSKo
    world_t w;
    // what the script found
    uint32_t found_ms[16];        // when each swap's printer was answering as the current record (0: never)
    uint32_t due_ms[16];          // and when it had to be
} swaps_t;

static int kind_of(int on)
{
    if (on == NOTHING) return -1;
    return on == IMAGEWRITER || on == ORIGINAL_IW ? PR_KIND_IMAGEWRITER : PR_KIND_STYLEWRITER;
}

static int raises_line(const swaps_t *s, int on)
{
    return on == IMAGEWRITER || on == ORIGINAL_IW || (on != NOTHING && s->sw_line);
}

// An answer naming `model`: a record's (1), or the first sighting of a model with none (0; the
// second in a row makes the record).
static int seen(swaps_t *s, int model)
{
    if (!s->listed[model] && s->pending != model) {
        s->pending = model;
        return 0;
    }
    s->listed[model] = 1;
    s->pending = NOTHING;
    s->current = model;
    return 1;
}

static int swaps_answer(swaps_t *s, port_act_t a, int line_ready, int *mine)
{
    world_t *w = &s->w;
    *mine = 0;
    switch (a) {
    case PA_ESCQ:
        w->escq++;
        if (is_sw(w)) {
            if (!w->deaf) w->escq_sw_awake++;
            w->deaf = 1;
            s->pending = NOTHING;                             // SILENT starts the count over
            return PR_HEARD_NONE;
        }
        if (w->on == IMAGEWRITER) {
            *mine = seen(s, IMAGEWRITER);
            return PR_HEARD_IMAGEWRITER;
        }
        // Nothing said: with the line up, an original ImageWriter record's answer
        // (pr_port_seen, SILENT); else nothing, and the count starts over.
        s->pending = NOTHING;
        if (line_ready && s->listed[ORIGINAL_IW]) s->current = ORIGINAL_IW;
        return PR_HEARD_NONE;
    case PA_PLAIN:
    case PA_STATUS:
        if (a == PA_STATUS) w->status++;
        if (!is_sw(w) || w->deaf) return PR_HEARD_NONE;
        *mine = seen(s, w->on);
        return PR_HEARD_STYLEWRITER;
    case PA_KICK:
        if (is_sw(w)) w->kicks_sw++;
        return PR_HEARD_NONE;
    default:
        return PR_HEARD_NONE;
    }
}

static void swaps_run(swaps_t *s, const swap_t *script, int n, uint32_t len_s)
{
    port_in_t in;
    memset(&in, 0, sizeof in);
    uint32_t t0 = 7000u * SEC;
    in.now_ms = in.last_plain_ms = in.last_status_ms = t0;
    int line = 0, kicked = 0, again = 0, next = 0;
    uint32_t down_since = t0, low_at = t0, kicked_at = 0;
    s->w.on = NOTHING;
    for (uint32_t ms = t0; ms < t0 + len_s * SEC; ms += again ? 0 : 500) {
        if (!again && next < n && ms == t0 + script[next].at_s * SEC) {
            int on = script[next].on;
            // A printer plugged in while a StyleWriter record answers is found once a status
            // check fails (within 10 s) and the look that follows asks; else within the 5 s
            // cadence, plus the repeat that makes a record.
            int sw_answers = kind_of(s->current) == PR_KIND_STYLEWRITER && in.answering;
            s->due_ms[next] = ms + (sw_answers ? 11 : 6) * SEC;
            s->w.on = on;
            s->w.deaf = 0;
            int line_now = raises_line(s, on);
            if (line) { port_line_down(&in); down_since = ms; low_at = ms; }
            if (line_now && (line || ms - low_at >= 1 * SEC)) { in.line_up_edge = 1; kicked = 0; }
            line = line_now;
            next++;
        }
        again = 0;
        in.now_ms = ms;
        in.kind = kind_of(s->current);
        in.iw1_listed = s->listed[ORIGINAL_IW];
        in.sw_listed = s->listed[SW_MINE] || s->listed[SW_OTHER];
        in.line_ready = line;
        in.want_kick = !line && ms - down_since >= 5 * SEC && (!kicked || ms - kicked_at >= 60 * SEC);
        port_tick(&in);
        port_act_t a = port_policy(&in);
        if (a == PA_PLAIN) s->w.plains++;
        if (a == PA_KICK) { kicked = 1; kicked_at = ms; }
        int mine;
        int heard = swaps_answer(s, a, line, &mine);
        port_after(&in, a, heard, mine);
        if (heard != PR_HEARD_NONE && !mine) in.confirm = a;  // PR_SEEN_AGAIN: the board asks again at once
        if (a == PA_ESCQ) again = 1;
        for (int i = 0; i < next; i++)
            if (!s->found_ms[i] && script[i].on != NOTHING && s->w.on == script[i].on &&
                s->current == script[i].on && in.answering)
                s->found_ms[i] = ms;
    }
}

static void check_swaps(const swaps_t *s, const swap_t *script, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (script[i].on == NOTHING) continue;
        int ok = s->found_ms[i] && s->found_ms[i] <= s->due_ms[i];
        CHECK(ok);
        if (!ok)
            fprintf(stderr, "  %s: %s plugged in at %u s was answering at %u s, due by %u s\n", name,
                    ON_PORT[script[i].on], (unsigned)script[i].at_s,
                    (unsigned)(s->found_ms[i] ? (s->found_ms[i] - 7000u * SEC) / SEC : 0),
                    (unsigned)((s->due_ms[i] - 7000u * SEC) / SEC));
    }
    CHECK_EQ_INT(s->w.escq_sw_awake, 0);
    CHECK_EQ_INT(s->w.kicks_sw, 0);
}

// Printers plugged in and out, one after another and in each other's place: each is answering
// as the current record within seconds (the design's timings: a StyleWriter within the 5 s
// cadence; an ImageWriter at its line-up's ESC ?, the board's 2 s settle aside; a printer
// plugged in while a StyleWriter record answers, within one status check and the look that
// follows it); no StyleWriter that would answer hears ESC ?; none hears CTRL-X.
static void test_swaps(void)
{
    // An empty list, the StyleWriters never raising the line: each printer makes its record
    // when it has answered twice.
    static const swap_t a[] = {
        { 5, IMAGEWRITER }, { 30, NOTHING }, { 40, SW_MINE }, { 70, SW_OTHER }, { 100, IMAGEWRITER },
        { 130, NOTHING }, { 150, SW_MINE }, { 180, NOTHING }, { 200, IMAGEWRITER }, { 230, NOTHING },
    };
    swaps_t s;
    memset(&s, 0, sizeof s);
    s.current = s.pending = NOTHING;
    swaps_run(&s, a, 10, 260);
    check_swaps(&s, a, 10, "an empty list");
    CHECK(s.listed[IMAGEWRITER] && s.listed[SW_MINE] && s.listed[SW_OTHER] && !s.listed[ORIGINAL_IW]);
    CHECK_EQ_INT(s.current, IMAGEWRITER);

    // An original ImageWriter record, the person's, listed first, and StyleWriters that raise
    // the line (Review Focus 1 in the loop): the port is StyleWriter-first throughout.
    static const swap_t b[] = {
        { 5, ORIGINAL_IW }, { 30, NOTHING }, { 40, SW_MINE }, { 70, IMAGEWRITER },
        { 110, ORIGINAL_IW }, { 140, SW_OTHER }, { 170, NOTHING },
    };
    memset(&s, 0, sizeof s);
    s.listed[ORIGINAL_IW] = 1;
    s.current = ORIGINAL_IW;
    s.pending = NOTHING;
    s.sw_line = 1;
    swaps_run(&s, b, 7, 200);
    check_swaps(&s, b, 7, "an original ImageWriter record first");
    CHECK(s.listed[IMAGEWRITER] && s.listed[SW_MINE] && s.listed[SW_OTHER]);

    // An ImageWriter II and a StyleWriter record, the ImageWriter's the oldest, StyleWriters
    // that raise the line: the two swapped for each other without a gap.
    static const swap_t c[] = {
        { 5, IMAGEWRITER }, { 30, SW_MINE }, { 60, IMAGEWRITER }, { 90, SW_OTHER }, { 120, NOTHING },
    };
    memset(&s, 0, sizeof s);
    s.listed[IMAGEWRITER] = s.listed[SW_MINE] = 1;
    s.current = IMAGEWRITER;
    s.pending = NOTHING;
    s.sw_line = 1;
    swaps_run(&s, c, 5, 150);
    check_swaps(&s, c, 5, "an ImageWriter II and a StyleWriter record");
    CHECK(s.listed[SW_OTHER]);
}

int main(void)
{
    test_invariants();
    test_said_other();
    test_said_other_person_look();
    test_confirm_repeats();
    test_stylewriter_answering();
    test_stylewriter_silent();
    test_imagewriter_answering();
    test_imagewriter_silent();
    test_quiet_cadence();
    test_original_imagewriter();
    test_no_record();
    test_kick();
    test_kick_prompt();
    test_wrap();
    test_stale_event();
    test_swap_sw_raises_line();
    test_line_blips();
    test_stylewriter_heard();
    test_original_look();
    test_original_boot();
    test_stop_looks();
    test_ten_minutes();
    test_swaps();
    TEST_END;
}
