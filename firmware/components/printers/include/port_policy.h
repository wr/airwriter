// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the idle printer port is asked, and when (W-1103, design "The port"). Pure: no clock,
// no I/O, no state. The board's port task builds a port_in_t, calls port_policy() and does
// what it says, so the rules are the host tests' (test_port_policy.c) and the port's wire is
// the board's.
//
// The port holds one printer, an ImageWriter or a StyleWriter. They speak at different
// speeds, so what reaches the wrong one has to be harmless; the questions are:
//
//   PA_ESCQ    ESC ? at 9600 with CTS, the ImageWriter's self-ID (printers.c, ribbon_check).
//   PA_PLAIN   the StyleWriter's plain question: the single byte ? at 57600, no flow control.
//   PA_STATUS  the StyleWriter's 1 and 2 queries at 57600, for a StyleWriter that answers.
//   PA_KICK    CTRL-X to an ImageWriter left busy while idle.
//
// The caller:
//  - calls only when it holds the job lock, and not while a job runs on the port. The policy
//    has no job input; it never has to say "wait".
//  - passes `look` and `line_up_edge` as events, not levels: it clears each once it has acted
//    on a result that answers it, or the same result comes back on the next call.
//  - does one thing per call, then calls again (every 500 ms, or at once after PA_ESCQ).
//  - sets `last_plain_ms` to the time of each PA_PLAIN it sends, and `last_status_ms` for each
//    PA_STATUS. When an ESC ? it sent got no answer it sets `last_plain_ms` to
//    port_plain_due(now_ms), so the plain question follows at once (the design's "at line-up
//    when ESC ? went unanswered"), and calls again.
//  - sets `answering` when the port printer answered its last question, and clears it when
//    it did not. An ImageWriter record's `answering` must also clear when its line goes down
//    and stays down: only a port that is not answering is asked the plain question, and the
//    kick follows only a plain question that got nothing, so a stuck ImageWriter that still
//    "answers" would never be kicked.
//  - passes `plain_unanswered` for the last plain question it asked: it got nothing. It
//    clears it when the line goes down, so each new busy spell gets one fresh plain question
//    before its first kick (a wanted kick with `plain_unanswered` clear asks one at once,
//    PA_PLAIN, not at the 60 s cadence; the kick follows on the next call, so a busy
//    ImageWriter is kicked about as soon as `want_kick` says, ~5 s).
#pragma once
#include <stdint.h>

typedef enum { PA_NONE, PA_ESCQ, PA_PLAIN, PA_STATUS, PA_KICK } port_act_t;

// How often the port's printer is asked, while it answers (the StyleWriter's status) and
// while it does not (the plain question).
enum { PORT_STATUS_EVERY_MS = 10000, PORT_PLAIN_EVERY_MS = 60000 };

typedef struct {
    int      kind;            // pr_kind_t of the port record, -1 none
    int      said_other;      // "Something else": the person said what is there is not a printer we ask
    int      answering;       // the port's printer answered its last question (meaningless with no record)
    int      line_ready;      // HSKo
    int      line_up_edge;    // HSKo came up after a second or more down (an event)
    int      look;            // boot, setup or the Printers section asked (an event)
    int      want_kick;       // printer_state_want_kick()
    uint32_t now_ms;          // any 32-bit millisecond clock; the policy only subtracts
    uint32_t last_plain_ms;   // when the plain question was last asked
    uint32_t last_status_ms;  // when the StyleWriter's status was last asked
    int      plain_unanswered;  // the last plain question got nothing
} port_in_t;

// A last_plain_ms that makes the plain question due at now_ms (and not a moment before the
// caller's next stamp), without a sentinel: the clock may wrap.
static inline uint32_t port_plain_due(uint32_t now_ms)
{
    return now_ms - PORT_PLAIN_EVERY_MS;
}

// The one thing to ask now, or PA_NONE. When several apply the order is:
//
//   1. PA_NONE      the person said "Something else": nothing is ever asked.
//   2. PA_STATUS    a StyleWriter record that answers (and only then), when its status
//                   is 10 s old or on a look. Nothing else is ever sent to it: no ESC ?,
//                   no plain question, no kick, whatever the line does.
//   3. PA_ESCQ      on a line-up edge, or on a look with the line ready. Every other
//                   record, and a StyleWriter record that has stopped answering (so an
//                   ImageWriter swapped in is noticed), and no record.
//   4. PA_PLAIN     while the port's printer is not answering (any record that does not
//                   answer, and no record): when the last one is 60 s old, or on a look,
//                   or at once when a kick is wanted and no plain question has been asked
//                   since the line went down (`want_kick`, `plain_unanswered` clear; not
//                   for a StyleWriter record). An ImageWriter that answers is never asked it.
//   5. PA_KICK      last: only with want_kick and a plain question that got nothing, and
//                   never for a StyleWriter record.
//
// The only difference between a record and no record is that a record's `answering` counts.
port_act_t port_policy(const port_in_t *in);
