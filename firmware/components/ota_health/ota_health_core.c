// SPDX-License-Identifier: GPL-3.0-or-later
// The rollback health gate, pure C (ota_health_core.h).
#include "ota_health_core.h"
#include <string.h>

void oh_init(oh_gate_t *g, const oh_timing_t *tm, int has_creds, uint32_t now_ms)
{
    memset(g, 0, sizeof *g);
    g->tm = *tm;
    g->boot_ms = now_ms;
    g->need = has_creds ? (OH_STA_UP | OH_IPP_UP | OH_WEB_UP) : (OH_AP_UP | OH_WEB_UP);
}

void oh_set(oh_gate_t *g, uint32_t bits, int on, uint32_t now_ms)
{
    g->up = on ? (g->up | bits) : (g->up & ~bits);
    int healthy = (g->up & g->need) == g->need;
    // A break restarts the settle: an image that loses the network a few
    // seconds in has not shown it can keep it.
    if (healthy && !g->healthy) g->healthy_ms = now_ms;
    g->healthy = (uint8_t)healthy;
}

oh_action_t oh_update(oh_gate_t *g, uint32_t now_ms)
{
    if (g->decided) return OH_WAIT;
    if (g->healthy && now_ms - g->healthy_ms >= g->tm.settle_ms) {
        g->decided = 1;
        return OH_MARK_VALID;
    }
    // Healthy and settling when the deadline passes is not a failure; give
    // it the rest of its settle.
    if (!g->healthy && now_ms - g->boot_ms >= g->tm.deadline_ms) {
        g->decided = 1;
        return OH_ROLLBACK;
    }
    return OH_WAIT;
}
