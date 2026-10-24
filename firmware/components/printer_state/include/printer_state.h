// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP glue around the pure printer_state_core reducer: a mutex-guarded
// instance on the esp_timer clock, plus a 100 ms poll task -- the core's
// timers are lazy, so something has to look for a state that changes on its
// own (offline after 3 s, a failure expiring).
#pragma once
#include "printer_state_core.h"

// Creates the lock, the reducer and the poll task. on_change runs on the poll
// task, outside the lock, once per change of display state (and once at
// start). It may be NULL. Call once, before any other function here.
void      printer_state_start(const ps_timing_t *tm,
                              void (*on_change)(const ps_view_t *v));

void      printer_state_post(ps_event_t ev);          // any task
void      printer_state_set_ap_stations(unsigned n);   // any task
void      printer_state_cable(uint8_t result);         // any task; PS_CABLE_*, W-1055
ps_view_t printer_state_view(void);                    // any task

// W-1039: whether to send the printer a CTRL-X now, and that one was sent.
// W-1088: judged on the port printer's own line, given here at every report.
void      printer_state_port_line(int ready);          // any task
int       printer_state_want_kick(void);               // any task
void      printer_state_kicked(void);                  // any task
