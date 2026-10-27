// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the idle printer port is asked, and when (W-1103, design "The port"). Pure: no clock,
// no I/O, no state of its own. The board's port task keeps a port_in_t, and each 500 ms calls
// port_tick, then port_policy, does what that says, and gives the answer to port_after; it
// calls port_line_down when the line drops. So the rules and the bookkeeping between calls are
// the host tests' (test_port_policy.c, which runs the whole loop against scripted printers),
// and the port's wire is the board's.
//
// The port holds one printer, an ImageWriter or a StyleWriter. They speak at different
// speeds, so what reaches the wrong one has to be harmless; the questions are:
//
//   PA_ESCQ    ESC ? at 9600 with CTS, the ImageWriter's self-ID (printers.c, ribbon_check).
//   PA_PLAIN   the StyleWriter's plain question: the single byte ? at 57600, no flow control.
//   PA_STATUS  the StyleWriter's 1 and 2 queries at 57600, for a StyleWriter that answers.
//   PA_KICK    CTRL-X to an ImageWriter left busy while idle.
//   PA_FULL    the StyleWriter's full sequence (reset, settle, ?), only to an original StyleWriter
//              record: once at start-up, and after the person's look that got nothing. The
//              original ignores the plain question until a reset (InkTalk's notes), so nothing
//              else finds it.
//
// A StyleWriter that has just been sent ESC ? may hear it as a command it does not know and
// stop listening until it is reset (swfake.py models it so), so a port whose printer is, or
// last answered as, a StyleWriter is asked the plain question first at a line-up or a look,
// and ESC ? only once that got nothing. And no question is asked over and over: the plain
// question goes at most once a minute on its own, and an ImageWriter record whose port a
// StyleWriter answered asks nothing more until a line-up or a look (the printer list holds
// that port's jobs until the person says, printers_core.h's pr_port_conflict).
//
// The caller:
//  - calls only when it holds the job lock, and not while a job runs on the port. The policy
//    has no job input; it never has to say "wait".
//  - sets `look` and `line_up_edge` as events, and `look_person` with `look` when the person
//    asked (Look for Printers); port_after clears them once a question answers them (for a
//    StyleWriter-first port, a line-up or a look with the line up takes the plain question and
//    then, if that got nothing, ESC ?), and sets `look` itself when a StyleWriter record that
//    was answering stops (its status got nothing): one look, the plain question and then
//    ESC ?, so an ImageWriter put in its place is found within one status interval. The caller
//    does the same when a job's question gets nothing from a record that was answering.
//  - does one thing per call, then calls again (every 500 ms, or at once after PA_ESCQ).
//  - fills `heard` from the printer list before each call (pr_port_heard), and passes each
//    answer to port_after: the family that answered (PR_HEARD_NONE for nothing) and whether it
//    was the record's own printer (its family, and for a StyleWriter its model).
//  - may set `answering` itself from other answers (a job's, a status read elsewhere), and
//    `last_status_ms` when something else asked `1` and `2`.
#pragma once
#include <stdint.h>

typedef enum { PA_NONE, PA_ESCQ, PA_PLAIN, PA_STATUS, PA_KICK, PA_FULL } port_act_t;

// How often the port's printer is asked, while it answers (the StyleWriter's status) and
// while it does not (the plain question).
enum { PORT_STATUS_EVERY_MS = 10000, PORT_PLAIN_EVERY_MS = 60000 };

typedef struct {
    // What is true now, each call.
    int      kind;            // pr_kind_t of the port record, -1 none
    int      no_self_id;      // the record is the original ImageWriter, which never answers ESC ?
    int      sw_original;     // the record is the original StyleWriter, which ignores ? until reset
    int      said_other;      // "Something else": the person said what is there is not a printer we ask
    int      line_ready;      // HSKo
    int      want_kick;       // printer_state_want_kick()
    int      heard;           // pr_heard_t: the family that last answered on the port
    uint32_t now_ms;          // any 32-bit millisecond clock; the policy only subtracts
    // Events: the caller sets them, port_after clears them.
    int      line_up_edge;    // HSKo came up after a second or more down
    int      look;            // boot, setup, the Printers section, a family flip, a stop
    int      look_person;     // ...and it is the person's Look for Printers
    // Kept between calls by port_tick, port_after and port_line_down.
    int      answering;       // the record's own printer answered its last question
    uint32_t last_plain_ms;   // when the plain question was last asked
    uint32_t last_status_ms;  // when the StyleWriter's status was last asked
    int      plain_asked;     // a plain question has been asked since the line went down
    int      plain_unanswered;  // the last plain question got nothing
    int      event_plain;     // the plain question asked for the pending line-up or look got nothing
    int      look_kick;       // a look got nothing: one CTRL-X may follow where a StyleWriter answered last
    int      full_due;        // owed an original StyleWriter record: start-up, or the person's look got nothing
} port_in_t;

// A last_plain_ms that makes the plain question due at now_ms (and not a moment before the
// caller's next stamp), without a sentinel: the clock may wrap.
static inline uint32_t port_plain_due(uint32_t now_ms)
{
    return now_ms - PORT_PLAIN_EVERY_MS;
}

// Before each port_policy call: an ImageWriter record whose line has been down for the kick
// delay (`want_kick`) is not answering any more, whatever it said before.
void port_tick(port_in_t *in);

// The one thing to ask now, or PA_NONE. When several apply the order is:
//
//   1. PA_NONE      the person said "Something else": nothing is ever asked.
//   2. PA_STATUS    a StyleWriter record that answers (and only then), when its status
//                   is 10 s old or on a look. Nothing else is ever sent to it: no ESC ?,
//                   no plain question, no kick, whatever the line does.
//   3. On a line-up edge, or a look with the line ready, for every other port: PA_PLAIN
//                   first where the port is StyleWriter-first (a StyleWriter record, a port
//                   that last answered as a StyleWriter, or an original ImageWriter record,
//                   which gives no self-ID), then PA_ESCQ once that got nothing
//                   (`event_plain`); PA_ESCQ alone everywhere else (an empty port, an
//                   ImageWriter record), so an ImageWriter swapped in is noticed.
//   4. PA_PLAIN     on a look with the line down, while the port's printer is not answering.
//   5. PA_FULL      `full_due` on an original StyleWriter record: set by the board at start-up,
//                   or by the person's look that got nothing from the plain question or ESC ?.
//                   Once.
//   6. PA_NONE      an ImageWriter record whose port a StyleWriter answered asks nothing on
//                   its own, and is kicked at most once a look that got nothing
//                   (`look_kick`), never on a cadence.
//   7. PA_PLAIN     while the port's printer is not answering (any record that does not
//                   answer, and no record): when the last one is 60 s old, or at once when
//                   a kick is wanted and none has been asked since the line went down
//                   (`want_kick`, `plain_asked` clear; not for a StyleWriter record).
//   8. PA_KICK      last: only with want_kick and a plain question since the line went down
//                   that got nothing, and never for a StyleWriter record.
//
// So a busy ImageWriter is kicked about as soon as want_kick says (~5 s): one plain question
// at once, the kick on the next call. The only difference between a record and no record is
// that a record's `answering` counts.
port_act_t port_policy(const port_in_t *in);

// What the question `act` got, at in->now_ms: `heard` the family that answered
// (PR_HEARD_NONE for nothing), `mine` 1 when that was the record's own printer. Keeps
// the bookkeeping: the events it answered are cleared (and with them `event_plain`), the
// stamps set, `answering` follows, `heard` takes the answer. An original ImageWriter record's
// ESC ? that got nothing with the line up is its answer (it gives no self-ID). An ESC ? that
// got nothing makes the plain question due at once, unless it was asked for this line-up or
// look already. A look that got nothing arms one kick (`look_kick`) and, for the person's look
// on an original StyleWriter record, the full sequence; an answer of any kind disarms both, as
// does the kick or the full sequence itself. A status that got nothing on the person's look
// keeps the look theirs.
void port_after(port_in_t *in, port_act_t act, int heard, int mine);

// HSKo went down: a new busy spell gets a fresh plain question before its first kick.
void port_line_down(port_in_t *in);
