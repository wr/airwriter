// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the idle printer port is asked, and when (W-1103, design "The port"): the pure
// policy behind the board's port task. A StyleWriter that answers is asked its status
// every 10 s and nothing else; a port whose printer is not answering is asked the plain
// question every 60 s; ESC ? waits for a line-up (or a look with the line ready); the
// ImageWriter's CTRL-X kick goes only to a port that is not a StyleWriter and whose plain
// question got nothing.
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

// The caller as the board runs it: every 500 ms it calls once, does what it is told and
// stamps the time it asked. counts[] is how often each action came. A StyleWriter that
// answers keeps answering; anything else never does.
typedef struct { int counts[5]; uint32_t at[5][64]; } tally_t;

static tally_t run(port_in_t in, uint32_t from_ms, uint32_t to_ms)
{
    tally_t t;
    memset(&t, 0, sizeof t);
    for (uint32_t ms = from_ms; ms < to_ms; ms += 500) {
        in.now_ms = ms;
        port_act_t a = port_policy(&in);
        if (t.counts[a] < 64) t.at[a][t.counts[a]] = ms;
        t.counts[a]++;
        if (a == PA_PLAIN) in.last_plain_ms = ms;
        if (a == PA_STATUS) in.last_status_ms = ms;
    }
    return t;
}

// Every combination of the flags, three times for the clock: due for neither question, for
// the status, for the plain one. What the policy must never do, whatever else is true.
static void test_invariants(void)
{
    for (int k = 0; k < N_KINDS; k++)
    for (unsigned f = 0; f < 128; f++)
    for (int due = 0; due < 3; due++) {
        port_in_t in = base(KINDS[k]);
        in.said_other = f & 1;
        in.answering = (f >> 1) & 1;
        in.line_ready = (f >> 2) & 1;
        in.line_up_edge = (f >> 3) & 1;
        in.look = (f >> 4) & 1;
        in.want_kick = (f >> 5) & 1;
        in.plain_unanswered = (f >> 6) & 1;
        if (due == 1) in.last_status_ms = in.now_ms - 10 * SEC;
        if (due == 2) in.last_plain_ms = in.now_ms - 60 * SEC;
        port_act_t a = port_policy(&in);
        int sw = KINDS[k] == PR_KIND_STYLEWRITER;
        int answers = KINDS[k] >= 0 && in.answering;

        if (in.said_other) CHECK_EQ_INT(a, PA_NONE);
        if (a == PA_KICK) CHECK(in.want_kick && in.plain_unanswered && !sw);
        if (a == PA_PLAIN) CHECK(!answers);
        // ...and only for a reason: a look, the 60 s cadence, or a wanted kick (R22) with no
        // plain question since the line went down, never for a StyleWriter record's kick.
        if (a == PA_PLAIN)
            CHECK(in.look || due == 2 || (in.want_kick && !in.plain_unanswered && !sw));
        if (a == PA_STATUS) CHECK(sw && answers);
        if (a == PA_ESCQ) CHECK(!(sw && answers) && (in.line_up_edge || (in.look && in.line_ready)));
        if (sw && answers) CHECK(a == PA_NONE || a == PA_STATUS);
        // An ImageWriter that answers is asked ESC ? only; the plain question is for the rest.
        if (KINDS[k] == PR_KIND_IMAGEWRITER && in.answering) CHECK(a != PA_PLAIN && a != PA_STATUS);
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

    // ESC ? at line-up, so an ImageWriter swapped in is noticed.
    in = base(PR_KIND_STYLEWRITER);
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    // It outranks a plain question that is due too.
    in = at(in, 600 * SEC, 600 * SEC);
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);

    // Never the kick, however long its line has been down and its plain question dead.
    in = base(PR_KIND_STYLEWRITER);
    in.want_kick = 1;
    in.plain_unanswered = 1;
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
    // ESC ? at line-up first, then the plain one.
    in = at(in, 60 * SEC, 0);
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.line_up_edge = 0;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
}

static void test_no_record(void)
{
    // A look with the line up asks ESC ?; the caller clears the look, and when that got no
    // answer stamps a plain question as due (port_plain_due), which the next call asks.
    port_in_t in = base(-1);
    in.look = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.look = 0;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);       // nothing is due yet...
    in.last_plain_ms = port_plain_due(in.now_ms);   // ...until the caller says it is
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    // With the line down a look asks the plain question straight away.
    in = base(-1);
    in.look = 1;
    in.line_ready = 0;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
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
    // A line-up edge asks ESC ?.
    in = base(-1);
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
}

static void test_kick(void)
{
    // The kick is the last resort: wanted, the plain question dead, nothing else to ask.
    for (int k = 0; k < N_KINDS; k++) {
        port_in_t in = base(KINDS[k]);
        in.want_kick = 1;
        in.plain_unanswered = 1;
        int sw = KINDS[k] == PR_KIND_STYLEWRITER;
        CHECK_EQ_INT(port_policy(&in), sw ? PA_NONE : PA_KICK);
        in.want_kick = 0;
        CHECK_EQ_INT(port_policy(&in), PA_NONE);
        // R22: no plain question since the line went down, so one is asked at once, however
        // recently the last one was; a StyleWriter record never gets that or the kick.
        in.want_kick = 1;
        in.plain_unanswered = 0;
        in = at(in, 1 * SEC, 0);
        CHECK_EQ_INT(port_policy(&in), sw ? PA_NONE : PA_PLAIN);
    }
    // An ImageWriter stuck busy at power-up, as the board runs it: its line is down, the
    // plain question comes due at 60 s and gets nothing, and the kick follows on the next
    // call and every call after, for printer_state to pace.
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in.last_plain_ms = in.now_ms - 60 * SEC;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    in.last_plain_ms = in.now_ms;
    in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_KICK);
    // ESC ? and the plain question both outrank it.
    in.line_up_edge = 1;
    CHECK_EQ_INT(port_policy(&in), PA_ESCQ);
    in.line_up_edge = 0;
    in.last_plain_ms = in.now_ms - 60 * SEC;
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
}

// R22: an ImageWriter left busy is kicked about as soon as it is wanted, not a plain-question
// cadence later. The caller clears plain_unanswered when the line goes down, and the busy
// spell gets one fresh plain question, then the kick.
static void test_kick_prompt(void)
{
    port_in_t in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in.plain_unanswered = 0;
    in = at(in, 1 * SEC, 0);                       // asked a second ago, for another spell
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    in.last_plain_ms = in.now_ms;                  // the caller stamps it; it got nothing
    in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_KICK);

    // The same with no record (a port nothing has been found on, left busy at power-up).
    in = base(-1);
    in.line_ready = 0;
    in.want_kick = 1;
    in = at(in, 1 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_PLAIN);
    in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_KICK);

    // A StyleWriter record never gets either on this path, answering or not.
    in = base(PR_KIND_STYLEWRITER);
    in.line_ready = 0;
    in.want_kick = 1;
    in = at(in, 1 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.plain_unanswered = 1;
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
    in.plain_unanswered = 0;
    in = at(in, 1 * SEC, 0);
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    // (If its flag is stale and the plain question had got nothing, it is kicked.)
    in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_KICK);

    // The person said "Something else": nothing, wanted or not.
    in = base(PR_KIND_IMAGEWRITER);
    in.said_other = 1;
    in.want_kick = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);
    in.plain_unanswered = 1;
    CHECK_EQ_INT(port_policy(&in), PA_NONE);

    // As the board runs it: the line drops at 100 s with the printer busy (its last plain
    // question was 5 s before, for an earlier spell, so the 60 s cadence is nowhere near
    // due), and want_kick comes true at 105 s. Calls every 500 ms: one plain question the
    // moment it is wanted, the kick on the very next call, then kicks for printer_state to
    // pace.
    in = base(PR_KIND_IMAGEWRITER);
    in.line_ready = 0;
    in.plain_unanswered = 0;                       // cleared when the line went down
    in.last_plain_ms = 95 * SEC;
    int plains = 0, kicks = 0;
    uint32_t first_plain = 0, first_kick = 0;
    for (uint32_t ms = 100 * SEC; ms < 130 * SEC; ms += 500) {
        in.now_ms = ms;
        in.want_kick = ms >= 105 * SEC;
        port_act_t a = port_policy(&in);
        if (a == PA_PLAIN) {
            if (!plains++) first_plain = ms;
            in.last_plain_ms = ms;
            in.plain_unanswered = 1;               // a busy ImageWriter says nothing
        }
        if (a == PA_KICK && !kicks++) first_kick = ms;
    }
    CHECK_EQ_INT(plains, 1);
    CHECK_EQ_INT(first_plain, 105 * SEC);
    CHECK_EQ_INT(first_kick, 105 * SEC + 500);
    CHECK(kicks >= 1);
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

int main(void)
{
    test_invariants();
    test_said_other();
    test_stylewriter_answering();
    test_stylewriter_silent();
    test_imagewriter_answering();
    test_imagewriter_silent();
    test_no_record();
    test_kick();
    test_kick_prompt();
    test_wrap();
    TEST_END;
}
