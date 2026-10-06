// SPDX-License-Identifier: GPL-3.0-or-later
//
// ESP glue around the pure printer_state_core reducer: a mutex-guarded
// instance plus a change callback, so any task can post an event and the
// rest of the firmware (status page, LED grid) can read or react to the
// derived state without touching FreeRTOS primitives itself.
#pragma once
#include "printer_state_core.h"

// Creates the mutex and the underlying reducer. Call once, before any task
// can call printer_state_post_ev(). on_change may be NULL; when set, it is
// invoked outside the lock, only when ps_post() derives a new state.
void printer_state_start(void (*on_change)(ps_state_t));

// Thread-safe: posts an event from any task.
void printer_state_post_ev(ps_event_t ev);

// Thread-safe: current derived state.
ps_state_t printer_state_get(void);
