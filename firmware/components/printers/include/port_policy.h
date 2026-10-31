// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the idle printer port is asked, and when (W-1103, design "The port"; W-1116, design
// "What the port is asked"). Pure: no clock, no I/O, no state of its own. The board's port
// task keeps a port_in_t, and each 500 ms calls port_tick, then port_policy, does what that
// says, and gives the answer to port_after; it calls port_line_down when the line drops. So the
// rules and the bookkeeping between calls are the host tests' (test_port_policy.c, which runs
// the whole loop against scripted printers), and the port's wire is the board's.
//
// The port holds a record per model, ImageWriters and StyleWriters, and at most one of them
// is answering: the one whose model answered last is the current record (printers_core.h,
// pr_port_current), and `kind` is its kind. What the list holds decides the order of the
// questions, not the current record alone. The two families speak at different speeds, so
// what reaches the wrong one has to be harmless; the questions are:
//
//   PA_ESCQ    ESC ? at 9600 with CTS, the ImageWriter's self-ID (printers.c, ribbon_check).
//   PA_PLAIN   the StyleWriter's plain question: the single byte ? at 57600, no flow control.
//   PA_STATUS  the StyleWriter's 1 and 2 queries at 57600, for a StyleWriter that answers.
//   PA_KICK    CTRL-X to an ImageWriter left busy while idle.
//   PA_FULL    the StyleWriter's full sequence (reset, settle, ?), only with an original
//              StyleWriter record listed: once at start-up, and after the person's look that
//              got nothing. The original ignores the plain question until a reset (InkTalk's
//              notes), so nothing else finds it.
//
// A StyleWriter that has just been sent ESC ? may hear it as a command it does not know and
// stop listening until it is reset (swfake.py models it so), so a port that is StyleWriter-
// first (any StyleWriter record listed, a port that last answered as a StyleWriter, or an
// original ImageWriter record listed, which gives no self-ID) is asked the plain question
// first at a line-up or a look, and ESC ? only once that got nothing. A port whose printer is
// not answering is asked the plain question every 5 s, whatever the list holds and whatever
// answered last, so a StyleWriter plugged in is found within seconds by someone who never
// added it; a different printer answering is that printer's record online and the last one
// offline (printers_core.h), never a notice.
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
//  - fills `kind` from the current record and the `_listed` flags from the list before each
//    call, `heard` from pr_port_heard, and passes each answer to port_after: the family that
//    answered (PR_HEARD_NONE for nothing) and whether it names a record (`mine`: the seen
//    call gave a record id). An answer that would change the list (PR_SEEN_AGAIN: a model
//    with no record, or one that would replace a Something else record or drop an Unknown
//    one; the first of the two in a row that make the change, R16) is not a record's yet:
//    the caller then sets `confirm` to the act, after port_after, and the next call repeats
//    it at once. It fills `iw_unnamed` (R20) when the current record is an ImageWriter II or
//    LQ that ESC ? has not named since its line came up, on a clean link; the policy then
//    re-asks only while nothing answers and a StyleWriter did not answer last.
//  - may set `answering` itself from other answers (a job's, a status read elsewhere), and
//    `last_status_ms` when something else asked `1` and `2`.
#pragma once
#include <stdint.h>

typedef enum { PA_NONE, PA_ESCQ, PA_PLAIN, PA_STATUS, PA_KICK, PA_FULL } port_act_t;

// How often the port's printer is asked, while it answers (the StyleWriter's status) and
// while it does not (the plain question).
enum { PORT_STATUS_EVERY_MS = 10000, PORT_PLAIN_EVERY_MS = 5000 };

typedef struct {
    // What is true now, each call.
    int      kind;            // pr_kind_t of the current port record, -1 none
    int      iw1_listed;      // an original ImageWriter record is listed: it never answers ESC ?
    int      sw_listed;       // a StyleWriter record is listed
    int      sw1_listed;      // an original StyleWriter record is listed: it ignores ? until reset
    int      said_other;      // "Something else": the person said what is there is not a printer we ask
    int      iw_unnamed;      // R20: the current record is an ImageWriter II or LQ whose ESC ? answer
                              // is not in since its line came up (a lost answer), the link clean and
                              // the line-up's own question run (the board holds it off in the settle)
    int      line_ready;      // HSKo
    int      want_kick;       // printer_state_want_kick()
    int      heard;           // pr_heard_t: the family that last answered on the port
    uint32_t now_ms;          // any 32-bit millisecond clock; the policy only subtracts
    // Events: the caller sets them, port_after clears them.
    int      line_up_edge;    // HSKo came up after a second or more down
    int      look;            // boot, setup, the Printers section, a family flip, a stop
    int      look_person;     // ...and it is the person's Look for Printers
    // Kept between calls by port_tick, port_after and port_line_down.
    int      answering;       // the current record's printer answered its last question
    uint32_t last_plain_ms;   // when the plain question was last asked
    uint32_t last_status_ms;  // when the StyleWriter's status was last asked
    int      plain_asked;     // a plain question has been asked since the line went down
    int      plain_unanswered;  // the last plain question got nothing
    int      event_plain;     // the plain question asked for the pending line-up or look got nothing
    int      full_due;        // owed an original StyleWriter record: start-up, or the person's look got nothing
    port_act_t confirm;       // an act to repeat at once: the caller sets it after port_after, which clears it
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
//   1. `confirm`     the act the caller wants repeated, whatever else is true.
//   2. PA_NONE      the person said "Something else": nothing of AirWriter's own. The
//                   person's own look (`look` with `look_person`) runs as any look does, rules
//                   3 to 5, and nothing follows it: not the plain question that follows an
//                   unanswered ESC ? elsewhere, not the cadence, not the full sequence (one
//                   owed waits until the Something else record goes).
//   3. PA_STATUS    a StyleWriter record that answers (and only then), when its status
//                   is 10 s old, on a look, or on a line-up edge: the line may be another
//                   printer's, so a StyleWriter that is gone turns into the stop's look at
//                   once (an ImageWriter in its place is found in about 1.5 s; the status
//                   bytes at 57600 are harmless to it, bench 2026-10-27). Nothing else is
//                   ever sent to it: no ESC ?, no plain question, no kick.
//   4. On a line-up edge, or a look with the line ready, for every other port: PA_PLAIN
//                   first where the port is StyleWriter-first (a StyleWriter record listed, a
//                   port that last answered as a StyleWriter, or an original ImageWriter
//                   record listed, which gives no self-ID), then PA_ESCQ once that got
//                   nothing (`event_plain`); PA_ESCQ alone everywhere else (an empty port,
//                   ImageWriter II or LQ records only), so an ImageWriter swapped in is noticed.
//                   R20: the same question, every 5 s on rule 7's clock, for an ImageWriter II
//                   or LQ record on the port that ESC ? has not named since its line came up
//                   (`iw_unnamed`, a lost answer) while its line is up and it does not answer,
//                   unless a StyleWriter answered last (a once-sighted one must not hear ESC ?);
//                   PA_ESCQ follows that re-ask's plain question when it got nothing.
//   5. PA_PLAIN     on a look with the line down, while the port's printer is not answering.
//   6. PA_FULL      `full_due` with an original StyleWriter record listed, while nothing
//                   answers: set by the board at start-up, or by the person's look that got
//                   nothing from the plain question or ESC ?. Once.
//   7. PA_PLAIN     while the port's printer is not answering (any record that does not
//                   answer, and no record): when the last one is 5 s old, or at once when
//                   a kick is wanted and none has been asked since the line went down
//                   (`want_kick`, `plain_asked` clear; not while the current record is a
//                   StyleWriter).
//   8. PA_KICK      last: only with want_kick and a plain question since the line went down
//                   that got nothing, and never while the current record is a StyleWriter.
//                   A StyleWriter that answers the plain question is never kicked: its answer
//                   is what the kick waits on.
//
// So a busy ImageWriter is kicked about as soon as want_kick says (~5 s): one plain question
// at once, the kick on the next call. The only difference between a record and no record is
// that a record's `answering` counts.
port_act_t port_policy(const port_in_t *in);

// What the question `act` got, at in->now_ms: `heard` the family that answered
// (PR_HEARD_NONE for nothing), `mine` 1 when that named a record. Clears `confirm` first.
// Keeps the bookkeeping: the events it answered are cleared (and with them `event_plain`),
// the stamps set, `answering` follows `mine`, `heard` takes the answer. With an original
// ImageWriter record listed, an ESC ? that got nothing with the line up is that record's
// answer (it gives no self-ID; the board makes it current). An ESC ? that got nothing makes
// the plain question due at once, unless it was asked for this line-up or look already. The
// person's look that got nothing owes the full sequence where an original StyleWriter record
// is listed; an answer of any kind, or the sequence itself, clears that. A status that got
// nothing on the person's look keeps the look theirs.
void port_after(port_in_t *in, port_act_t act, int heard, int mine);

// HSKo went down: a new busy spell gets a fresh plain question before its first kick.
void port_line_down(port_in_t *in);
