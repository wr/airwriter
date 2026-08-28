// SPDX-License-Identifier: GPL-3.0-or-later
// Device-state reducer: flags in, one derived state out. Pure C on purpose --
// the ESP glue and the host tests share this file. Priority does the work;
// there is no transition table to get wrong.
#include "printer_state_core.h"
#include <string.h>

void ps_init(ps_t *ps) { memset(ps, 0, sizeof *ps); }

ps_state_t ps_state(const ps_t *ps)
{
    if (ps->setup)     return PS_SETUP;
    if (!ps->wifi_up)  return ps->connecting ? PS_WIFI_CONNECTING : PS_BOOT;
    if (ps->blocked)   return PS_BLOCKED;
    if (ps->printing)  return PS_PRINTING;
    if (ps->error)     return PS_ERROR;
    return PS_READY;
}

ps_state_t ps_post(ps_t *ps, ps_event_t ev)
{
    switch (ev) {
    case PS_EV_WIFI_CONNECTING: ps->connecting = 1;                  break;
    case PS_EV_WIFI_UP:         ps->wifi_up = 1; ps->connecting = 0; break;
    case PS_EV_SETUP_MODE:      ps->setup = 1;                       break;
    case PS_EV_JOB_START:       ps->printing = 1; ps->error = 0;     break;
    case PS_EV_JOB_DONE:        ps->printing = 0;                    break;
    case PS_EV_JOB_FAIL:        ps->printing = 0; ps->error = 1;     break;
    case PS_EV_PRINTER_BLOCKED: ps->blocked = 1;                     break;
    case PS_EV_PRINTER_CLEARED: ps->blocked = 0;                     break;
    }
    return ps_state(ps);
}

const char *ps_name(ps_state_t st)
{
    switch (st) {
    case PS_BOOT:            return "boot";
    case PS_SETUP:           return "setup";
    case PS_WIFI_CONNECTING: return "connecting";
    case PS_BLOCKED:         return "blocked";
    case PS_PRINTING:        return "printing";
    case PS_ERROR:           return "error";
    case PS_READY:           return "ready";
    }
    return "?";
}
