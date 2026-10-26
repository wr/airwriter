// SPDX-License-Identifier: GPL-3.0-or-later
#include "port_policy.h"
#include "printers_core.h"

// Wrap-safe: the difference of two 32-bit millisecond times, as unsigned, whichever side of
// the wrap each is on. A stamp is the caller's own now_ms, never later than the next one.
static int due(uint32_t now_ms, uint32_t last_ms, uint32_t every_ms)
{
    return (uint32_t)(now_ms - last_ms) >= every_ms;
}

port_act_t port_policy(const port_in_t *in)
{
    if (in->said_other) return PA_NONE;

    int sw = in->kind == PR_KIND_STYLEWRITER;
    int answers = in->kind >= 0 && in->answering;

    if (sw && answers)
        return in->look || due(in->now_ms, in->last_status_ms, PORT_STATUS_EVERY_MS)
               ? PA_STATUS : PA_NONE;

    if (in->line_up_edge || (in->look && in->line_ready)) return PA_ESCQ;

    // R22: a busy ImageWriter is kicked about as soon as it always was, so a wanted kick
    // with no plain question since the line went down asks one at once; the kick follows.
    int kick_asks = in->want_kick && !in->plain_unanswered && !sw;
    if (!answers && (in->look || kick_asks ||
                     due(in->now_ms, in->last_plain_ms, PORT_PLAIN_EVERY_MS)))
        return PA_PLAIN;

    if (in->want_kick && in->plain_unanswered && !sw) return PA_KICK;
    return PA_NONE;
}
