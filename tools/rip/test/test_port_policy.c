// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the idle printer port is asked, and when (W-1103, design "The port"): the pure
// policy behind the board's port task, and the bookkeeping between its calls. A StyleWriter
// that answers is asked its status every 10 s and nothing else; a port whose printer is not
// answering is asked the plain question every 60 s; ESC ? waits for a line-up (or a look with
// the line ready), after the plain question where the port is, or last answered as, a
// StyleWriter; the ImageWriter's CTRL-X kick goes only to a port that is not a StyleWriter
// and whose plain question got nothing. The last part runs the whole loop for ten minutes
// against scripted printers, every record against every printer: no question loops, and no
// StyleWriter hears ESC ? while it would answer the plain question, or the kick at all.
#include "test_util.h"
#include "port_policy.h"
#include "printers_core.h"

#define SEC 1000u   // ms

static port_in_t base(int kind)
{
    port_in_t i;
    memset(&i, 0, sizeof i);
    i.kind = kind;
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

static int sw_first_of(const port_in_t *in)
{
    return in->kind == PR_KIND_STYLEWRITER || in->heard == PR_HEARD_STYLEWRITER ||
           (in->kind == PR_KIND_IMAGEWRITER && in->no_self_id);
}

// Every combination of the flags, three times for the clock: due for neither question, for
// the status, for the plain one. What the policy must never do, whatever else is true.
static void test_invariants(void)
{
    for (int k = 0; k < N_KINDS; k++)
    for (unsigned f = 0; f < 16384; f++)
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
        in.no_self_id = (f >> 9) & 1;
        in.look_kick = (f >> 10) & 1;
        in.sw_original = (f >> 11) & 1;
        in.look_person = (f >> 12) & 1;
        in.full_due = (f >> 13) & 1;
        in.heard = heard;
        if (due == 1) in.last_status_ms = in.now_ms - 10 * SEC;
        if (due == 2) in.last_plain_ms = in.now_ms - 60 * SEC;
        port_act_t a = port_policy(&in);
        int sw = KINDS[k] == PR_KIND_STYLEWRITER;
        int iw = KINDS[k] == PR_KIND_IMAGEWRITER;
        int answers = KINDS[k] >= 0 && in.answering;
        int event = in.line_up_edge || (in.look && in.line_ready);

        int quiet = iw && heard == PR_HEARD_STYLEWRITER;
        if (in.said_other) CHECK_EQ_INT(a, PA_NONE);
        // The kick: wanted, after a plain question since the line went down that got
        // nothing, never for a StyleWriter record; on an ImageWriter record whose port a
        // StyleWriter answered, only once after a look that got nothing.
        if (a == PA_KICK)
            CHECK(in.want_kick && !sw &&
                  (quiet ? in.look_kick : in.plain_asked && in.plain_unanswered));
        // The full sequence: only after the person's look on an original StyleWriter record whose
        // port an ImageWriter answered, and only once that look's questions got nothing.
        if (a == PA_FULL)
            CHECK(in.full_due && sw && in.sw_original && heard == PR_HEARD_IMAGEWRITER && !event &&
                  !in.look);
        // The plain question only for a reason: a line-up or a look that takes it first, a
        // look with the line down, the 60 s cadence, or a wanted kick with none since the
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
        // An ImageWriter record whose port a StyleWriter answered asks nothing on its own.
        if (iw && heard == PR_HEARD_STYLEWRITER && !event && !in.look) CHECK(a == PA_NONE || a == PA_KICK);
        // An ImageWriter that answers is never asked the plain question on its own.
        if (iw && in.answering && !event) CHECK(a != PA_PLAIN && a != PA_STATUS);
    }
}

static void test_said_other(void)
{
    for (int k = 0; k < N_KINDS; k++) {
        port_in_t in = base(KINDS[k]);
        in.said_other = 1;
        in.answering = 1;
        in.line_up_edge = 1;
        in.look = 1;
        in.want_kick = 1;
        in.plain_unanswered = 1;
        in.plain_asked = 1;
        in = at(in, 600 * SEC, 600 * SEC);
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
    }
    port_in_t in = base(-1);
    in.said_other = 1;
    tally_t t = run(in, 0, 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_NONE], 1200);
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
    // The plain question every 60 s, and no status.
    in = at(in, 59 * SEC + 999, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in = at(in, 60 * SEC, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_in_t q = base(PR_KIND_STYLEWRITER);
    q.last_plain_ms = 1000 * SEC;
    tally_t t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 5);
    CHECK_EQ_INT(t.at[PA_PLAIN][0], 1060 * SEC);
    CHECK_EQ_INT(t.at[PA_PLAIN][4] - t.at[PA_PLAIN][3], 60 * SEC);
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
    in = at(in, 60 * SEC, 0);
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
    in = at(in, 59 * SEC + 999, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in = at(in, 60 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_in_t q = base(PR_KIND_IMAGEWRITER);
    tally_t t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 5);
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

// The original ImageWriter gives no self-ID: an ESC ? that gets nothing with its line up is it
// answering, so it is not asked the plain question every minute for as long as it is on.
// Being StyleWriter-first, a line-up asks the plain question first, so a StyleWriter in its
// place answers before it hears ESC ?.
static void test_original_imagewriter(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.no_self_id = 1;
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 1);
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);
    tally_t t = run(in, in.now_ms, in.now_ms + 600 * SEC);
    CHECK_EQ_INT(t.counts[PA_NONE], 1200);
    // With its line down, ESC ? getting nothing is not an answer.
    in = base(PR_KIND_IMAGEWRITER);
    in.no_self_id = 1;
    in.line_ready = 0;
    in.event_plain = 1;
    in.line_up_edge = 1;
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_NONE);
    // A II's record is not: its silence is no answer.
    in = base(PR_KIND_IMAGEWRITER);
    in.line_up_edge = 1;
    port_after(&in, PA_ESCQ, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(in.answering, 0);
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
    // And every 60 s with nothing else going on, line up or not.
    port_in_t q = base(-1);
    tally_t t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 5);
    q.line_ready = 0;
    t = run(q, 1000 * SEC, 1301 * SEC);
    CHECK_EQ_INT(t.counts[PA_PLAIN], 5);
    // "Answering" means nothing with no record.
    in = base(-1);
    in.answering = 1;
    in = at(in, 60 * SEC, 0);
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
    // plain question comes due at 60 s and gets nothing, and the kick follows on the next
    // call and every call after, for printer_state to pace.
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in.plain_asked = 1;
    in.last_plain_ms = in.now_ms - 60 * SEC;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    port_after(&in, PA_PLAIN, PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_KICK);
    // ESC ? and the plain question both outrank it.
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.line_up_edge = 0;
    in.last_plain_ms = in.now_ms - 60 * SEC;
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
// one fresh plain question, then the kick; one that a StyleWriter answers gets neither again.
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
    // question was 5 s before, for an earlier spell, so the 60 s cadence is nowhere near
    // due), and want_kick comes true at 105 s. Calls every 500 ms: one plain question the
    // moment it is wanted, the kick on the very next call, then kicks for printer_state to
    // pace.
    in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.last_plain_ms = 95 * SEC;
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
    CHECK_EQ_INT(plains, 1);
    CHECK_EQ_INT(first_plain, 105 * SEC);
    CHECK_EQ_INT(first_kick, 105 * SEC + 500);
    CHECK(kicks >= 1);

    // An ImageWriter record with a StyleWriter on its port, which does not raise the line, so
    // the kick is wanted for as long as it is there: the wanted kick asks one plain question,
    // the StyleWriter answers it, and nothing more is asked: no plain question every call,
    // and no CTRL-X to a StyleWriter.
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
    CHECK_EQ_INT(plains, 1);
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
    in.now_ms = 0xFFFFFF00u + 60 * SEC - 1;     // wraps past zero
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

// An ImageWriter record whose port a StyleWriter answered: CTRL-X at most once per look that got
// nothing, never on a cadence. The StyleWriter is switched off at 2 min, the person looks at
// 3 min (nothing answers), it is switched on again at 4 min with its line down; the kick that
// look allowed goes once, and none after.
static void test_kick_after_look(void)
{
    port_in_t in;
    memset(&in, 0, sizeof in);
    in.kind = PR_KIND_IMAGEWRITER;
    in.heard = PR_HEARD_STYLEWRITER;
    in.line_ready = 0;
    uint32_t t = 1000 * SEC;
    in.now_ms = in.last_plain_ms = t;
    int sw_on = 1, kicks = 0, kicks_to_sw = 0, kicked = 0;
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
        if (a == PA_PLAIN && sw_on) heard = PR_HEARD_STYLEWRITER;
        if (a == PA_KICK) {
            kicked = 1;
            kicked_at = ms;
            kicks++;
            if (sw_on) kicks_to_sw++;
        }
        port_after(&in, a, heard, 0);
    }
    CHECK_EQ_INT(kicks, 1);
    CHECK_EQ_INT(kicks_to_sw, 0);
    CHECK_EQ_INT(in.heard, PR_HEARD_STYLEWRITER);
    // A look that a printer answered allows none.
    in.look = in.look_person = 1;
    port_after(&in, port_policy(&in), PR_HEARD_STYLEWRITER, 0);
    in.want_kick = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
}

// An original StyleWriter record whose port an ImageWriter answered: its own printer ignores
// the plain question until a reset, so only the person's Look for Printers can find it again.
// The look asks the plain question and ESC ? first, and only when nothing answers sends the full
// sequence, once.
static void test_original_look(void)
{
    // Its line up: the plain question, ESC ?, then the full sequence; the original answers it.
    port_in_t in = base(PR_KIND_STYLEWRITER);
    in.sw_original = 1;
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
    in.sw_original = 1;
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
    in.sw_original = 1;
    in.heard = PR_HEARD_IMAGEWRITER;
    in.look = in.look_person = 1;
    port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    port_after(&in, PA_ESCQ, PR_HEARD_IMAGEWRITER, 0);
    CHECK_EQ_INT(in.full_due, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // Never for another model, nor for an original record whose port no ImageWriter answered.
    static const struct { int original, heard; } no[] = {
        { 0, PR_HEARD_IMAGEWRITER }, { 1, PR_HEARD_STYLEWRITER }, { 1, PR_HEARD_NONE },
    };
    for (size_t i = 0; i < sizeof no / sizeof no[0]; i++) {
        in = base(PR_KIND_STYLEWRITER);
        in.sw_original = no[i].original;
        in.heard = no[i].heard;
        in.line_ready = 0;
        in.look = in.look_person = 1;
        port_after(&in, port_policy(&in), PR_HEARD_NONE, 0);
        CHECK_EQ_INT(in.full_due, 0);
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
    }
    // An ImageWriter record never gets it, whatever the flags say.
    in = base(PR_KIND_IMAGEWRITER);
    in.sw_original = 1;
    in.heard = PR_HEARD_IMAGEWRITER;
    in.full_due = 1;
    CHECK(port_policy(&in) != PA_FULL);
}

// A StyleWriter record that was answering and stops gets one look at once: the plain question,
// then ESC ? if that gets nothing. So an ImageWriter put in its place with its line already up
// (behind an A/B switch, its line-up passed while the StyleWriter still answered) is found within
// one status interval, and the record's jobs are refused (heard: an ImageWriter).
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
    CHECK_EQ_INT(in.heard, PR_HEARD_IMAGEWRITER);   // pr_port_conflict: the record's jobs refused
    CHECK_EQ_INT(escq, 1);                          // once per stop, not again
    CHECK_EQ_INT(sw_heard_escq, 0);
    CHECK(plains <= 1 + 7);                         // the stop's, then the 60 s cadence

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
    CHECK_EQ_INT(t.counts[PA_PLAIN], 4);           // 60, 120, 180, 240 s after
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
    int plains, escq, escq_sw_awake, kicks_sw, status, looks, quiet_kicks;
    uint32_t plain_at[64];
    uint32_t min_gap;             // the least time between two plain questions not asked for a look or a line-up
} world_t;

static int is_sw(const world_t *w) { return !w->off && (w->on == SW_MINE || w->on == SW_OTHER); }

// Who answers `a`: the family, and *mine whether it is the record's own printer.
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

// The board's loop: every 500 ms, or at once after ESC ? (`again`), with printer_state's kick
// timing (down 5 s, then at most every 60 s) and the first plain question 60 s after start.
// With `events`, the printer is switched off at 2 min, the person looks at 3 min, it is switched
// on again at 4 min (its line up again if it was, a line-up), and the person looks at 6 min.
static world_t ten_minutes(int kind, int no_self_id, int on, int line, int boot_look, int events)
{
    world_t w;
    memset(&w, 0, sizeof w);
    w.on = on;
    w.line = line;
    w.looks = boot_look;
    w.min_gap = 0xFFFFFFFFu;
    port_in_t in;
    memset(&in, 0, sizeof in);
    in.kind = kind;
    in.no_self_id = no_self_id;
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
            if (kind == PR_KIND_IMAGEWRITER && in.heard == PR_HEARD_STYLEWRITER) w.quiet_kicks++;
        }
        int mine;
        int heard = world_answer(&w, &in, a, &mine);
        port_after(&in, a, heard, mine);
        if (a == PA_ESCQ) again = 1;
    }
    return w;
}

// Every record against every printer, its line up or down, with a look at boot or not:
// the plain question at most once a minute of its own accord; no StyleWriter that would answer
// it hears ESC ? first where the port is StyleWriter-first; no StyleWriter ever hears CTRL-X;
// a StyleWriter that answers as the record's is asked its status.
static void test_ten_minutes(void)
{
    static const struct { int kind, no_self_id; const char *name; } recs[] = {
        { -1, 0, "no record" }, { PR_KIND_UNKNOWN, 0, "an unknown record" },
        { PR_KIND_IMAGEWRITER, 0, "an ImageWriter II record" },
        { PR_KIND_IMAGEWRITER, 1, "an original ImageWriter record" },
        { PR_KIND_STYLEWRITER, 0, "a StyleWriter record" },
    };
    for (size_t r = 0; r < sizeof recs / sizeof recs[0]; r++)
    for (int on = 0; on < N_ON_PORT; on++)
    for (int line = 0; line <= 1; line++)
    for (int look = 0; look <= 1; look++)
    for (int ev = 0; ev <= 1; ev++) {
        world_t w = ten_minutes(recs[r].kind, recs[r].no_self_id, on, line, look, ev);
        int sw = recs[r].kind == PR_KIND_STYLEWRITER, iw = recs[r].kind == PR_KIND_IMAGEWRITER;
        int sw_on = on == SW_MINE || on == SW_OTHER;
        int fail = 0;
        // at most once a minute, after whatever a look or a line-up asked
        if (w.min_gap < 60 * SEC) fail = 1;
        if (w.plains > (ev ? 16 : 12)) fail = 1;
        // CTRL-X never reaches a StyleWriter; with the printer switched off and on and the person
        // looking, an ImageWriter record whose port a StyleWriter answered kicks at most once a
        // look (no record, or an unknown one, would be the StyleWriter's record by then).
        if (!ev && w.kicks_sw) fail = 1;
        if (ev && (sw || iw) && w.kicks_sw > w.looks) fail = 1;
        if (w.quiet_kicks > w.looks) fail = 1;
        if (sw && (w.kicks_sw || w.quiet_kicks)) fail = 1;
        // A StyleWriter record never sends ESC ? to a StyleWriter that would have answered.
        if (sw && w.escq_sw_awake) fail = 1;
        // The record's own StyleWriter, answering: one plain question, then its status.
        if (!ev && sw && on == SW_MINE && (w.plains != 1 || w.status < 50)) fail = 1;
        if (ev && sw && on == SW_MINE && w.status < 30) fail = 1;
        // An ImageWriter record with a StyleWriter answering on its port: one plain question.
        if (!ev && iw && !line && sw_on && w.plains != 1) fail = 1;
        // An ImageWriter that answers is never asked it; the original only at its line-up.
        if (!ev && iw && line && on == IMAGEWRITER && !recs[r].no_self_id && w.plains) fail = 1;
        if (!ev && recs[r].no_self_id && line && on == ORIGINAL_IW && w.plains > 1) fail = 1;
        CHECK(!fail);
        if (fail)
            fprintf(stderr, "  %s, %s on the port, line %s, %s%s: %d plain (closest %u ms apart), "
                    "%d ESC ? (%d to a listening StyleWriter), %d kicks to a StyleWriter, %d kicks "
                    "while a StyleWriter answered last, %d looks, %d status\n",
                    recs[r].name, ON_PORT[on], line ? "up" : "down", look ? "a look at boot" : "no look",
                    ev ? ", switched off, looked, on, looked" : "", w.plains, (unsigned)w.min_gap,
                    w.escq, w.escq_sw_awake, w.kicks_sw, w.quiet_kicks, w.looks, w.status);
    }
}

int main(void)
{
    test_invariants();
    test_said_other();
    test_stylewriter_answering();
    test_stylewriter_silent();
    test_imagewriter_answering();
    test_imagewriter_silent();
    test_original_imagewriter();
    test_no_record();
    test_kick();
    test_kick_prompt();
    test_wrap();
    test_stale_event();
    test_kick_after_look();
    test_original_look();
    test_stop_looks();
    test_ten_minutes();
    TEST_END;
}
