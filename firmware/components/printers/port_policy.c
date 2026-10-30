// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_policy.h"
#include "printers_core.h"

// Wrap-safe: the difference of two 32-bit millisecond times, as unsigned, whichever side of
// the wrap each is on. A stamp is the caller's own now_ms, never later than the next one.
static int due(uint32_t now_ms, uint32_t last_ms, uint32_t every_ms)
{
    return (uint32_t)(now_ms - last_ms) >= every_ms;
}

void port_tick(port_in_t *in)
{
    if (in->want_kick && in->kind == PR_KIND_IMAGEWRITER) in->answering = 0;
}

port_act_t port_policy(const port_in_t *in)
{
    // An answer the caller wants confirmed (one sighting of a model with no record): the
    // same question again, before anything else.
    if (in->confirm != PA_NONE) return in->confirm;

    int sw = in->kind == PR_KIND_STYLEWRITER;
    int answers = in->kind >= 0 && in->answering;
    int person_look = in->look && in->look_person;

    // "Something else": nothing of AirWriter's own. The person's own look asks as any look
    // does, below, and nothing follows it.
    if (in->said_other && !person_look) return PA_NONE;

    if (sw && answers)
        return in->look || due(in->now_ms, in->last_status_ms, PORT_STATUS_EVERY_MS)
               ? PA_STATUS : PA_NONE;

    // A StyleWriter may stop listening once it has heard ESC ?, so a port with a StyleWriter
    // record, or that last answered as a StyleWriter, takes the plain question first; so does
    // one with an original ImageWriter record, which gives no self-ID and is found by its
    // line alone.
    int sw_first = in->sw_listed || in->heard == PR_HEARD_STYLEWRITER || in->iw1_listed;
    if (in->line_up_edge || (in->look && in->line_ready))
        return sw_first && !in->event_plain ? PA_PLAIN : PA_ESCQ;
    if (in->look && !answers) return PA_PLAIN;
    if (in->said_other) return PA_NONE;

    // An original StyleWriter record at start-up, or the person's look that found nothing on
    // the port: the full sequence, which the original answers once it is reset. Not while
    // another printer answers.
    if (in->full_due && in->sw1_listed && !answers) return PA_FULL;

    // CTRL-X follows only a plain question since the line went down that got nothing, and
    // never while the current record is a StyleWriter.
    int kick = in->want_kick && in->plain_asked && in->plain_unanswered && !sw;

    // A wanted kick with no plain question since the line went down asks one at once, so a
    // busy ImageWriter is kicked about as soon as it always was; the kick follows.
    int kick_asks = in->want_kick && !in->plain_asked && !sw;
    if (!answers && (kick_asks || due(in->now_ms, in->last_plain_ms, PORT_PLAIN_EVERY_MS)))
        return PA_PLAIN;
    return kick ? PA_KICK : PA_NONE;
}

// A look is over and nothing answered it: the person's look owes the full sequence where an
// original StyleWriter record is listed (one owed since start-up stays owed).
static void look_found_nothing(port_in_t *in)
{
    if (in->look_person && in->sw1_listed) in->full_due = 1;
}

static void events_over(port_in_t *in)
{
    in->look = in->look_person = in->line_up_edge = in->event_plain = 0;
}

void port_after(port_in_t *in, port_act_t act, int heard, int mine)
{
    in->confirm = PA_NONE;
    // Any answer: the port has a printer that speaks, so the full sequence is not wanted.
    if (heard != PR_HEARD_NONE) in->full_due = 0;
    switch (act) {
    case PA_ESCQ: {
        int asked_plain = in->event_plain, was_look = in->look;
        // The original ImageWriter has no self-ID: with its record listed, the line up and
        // silence are its answer (the board makes its record current).
        if (heard == PR_HEARD_NONE && in->iw1_listed && in->line_ready) {
            heard = PR_HEARD_IMAGEWRITER;
            mine = 1;
            in->full_due = 0;
        }
        if (heard != PR_HEARD_NONE) {
            in->heard = heard;
            in->answering = mine ? 1 : 0;
        } else {
            in->answering = 0;
            // Nothing said: the plain question follows at once, if this line-up or look
            // has not had it already.
            if (!asked_plain) in->last_plain_ms = port_plain_due(in->now_ms);
            else if (was_look) look_found_nothing(in);
        }
        events_over(in);
        break;
    }
    case PA_PLAIN:
        in->last_plain_ms = in->now_ms;
        in->plain_asked = 1;
        in->plain_unanswered = heard == PR_HEARD_NONE;
        if (heard != PR_HEARD_NONE) {
            in->heard = heard;
            in->answering = mine ? 1 : 0;
            events_over(in);                                      // answered: no ESC ?
        } else if (in->line_up_edge || (in->look && in->line_ready)) {
            in->event_plain = 1;                                  // ESC ? next
        } else {
            if (in->look) look_found_nothing(in);                 // a look with the line down
            events_over(in);
        }
        break;
    case PA_STATUS: {
        // A StyleWriter record that was answering and now says nothing: one look, at once, still
        // the person's if theirs asked the status.
        int stopped = heard == PR_HEARD_NONE && in->answering;
        int person = in->look && in->look_person;
        in->last_status_ms = in->now_ms;
        events_over(in);
        in->look = stopped;
        in->look_person = stopped && person;
        in->answering = heard != PR_HEARD_NONE;
        if (heard != PR_HEARD_NONE) in->heard = heard;
        break;
    }
    case PA_FULL:
        in->full_due = 0;
        events_over(in);
        if (heard != PR_HEARD_NONE) {
            in->heard = heard;
            in->answering = mine ? 1 : 0;
        }
        break;
    case PA_KICK:
    case PA_NONE:
        break;
    }
}

void port_line_down(port_in_t *in)
{
    in->plain_asked = 0;
    in->plain_unanswered = 0;
}
