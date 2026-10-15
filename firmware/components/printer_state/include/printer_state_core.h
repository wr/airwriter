// SPDX-License-Identifier: GPL-3.0-or-later
// Pure reducer: events and the clock in, one display state out. No FreeRTOS,
// no I/O -- printer_state.c wraps it with a mutex and a poll task, and the host
// tests drive it with a fake clock. Every timer is evaluated lazily in
// ps_update() against the caller's `now`. Times are uint32 milliseconds and
// only ever compared by subtraction, so the 49-day wrap is harmless.
// docs/superpowers/specs/2026-10-14-status-language-design.md
#pragma once
#include <stdint.h>

typedef enum {                 // priority order, highest first
    PS_BOOTING = 0, PS_SETUP, PS_JOINING, PS_ATTENTION,
    PS_FAILED, PS_PRINTING, PS_OFFLINE, PS_READY,
} ps_state_t;

typedef enum {
    PS_EV_WIFI_SETUP_AP,    // became the setup access point
    PS_EV_WIFI_JOINING,     // started joining the saved network
    PS_EV_WIFI_UP,          // got an address
    PS_EV_WIFI_DOWN,        // lost the network after having it
    PS_EV_LINE_READY,       // HSKo asserted
    PS_EV_LINE_NOT_READY,   // HSKo deasserted
    PS_EV_JOB_START,
    PS_EV_JOB_DONE,
    PS_EV_JOB_FAILED,
    PS_EV_ACTIVITY,         // a person did something: wake the panel
} ps_event_t;

typedef struct {
    uint32_t boot_ms;              // length of the boot animation
    uint32_t offline_debounce_ms;  // idle and not ready this long -> offline
    uint32_t stall_attention_ms;   // mid-job and not ready this long -> attention
    uint32_t failed_hold_ms;       // how long a failed job stays on the panel
    uint32_t sleep_after_ms;       // ready or offline with no activity -> asleep
} ps_timing_t;

#define PS_TIMING_DEFAULT { 3000, 3000, 30000, 60000, 300000 }

typedef struct {
    ps_timing_t tm;
    uint32_t    boot_at, line_since, failed_at, activity_at, state_since;
    uint8_t     setup, wifi_up, line_ready, job, failed, ap_stations, greeted;
    ps_state_t  last;
} ps_t;

typedef struct {
    ps_state_t state;
    uint8_t    phone_joined;  // SETUP only: a station is on the access point
    uint8_t    greeted;       // READY only: entered straight from JOINING or BOOTING,
                              // i.e. Wi-Fi just connected -- the panel's green moment
    uint8_t    asleep;        // READY or OFFLINE only, after sleep_after_ms idle
    uint32_t   since_ms;      // time spent in `state` so far
    uint32_t   asleep_ms;     // time since falling asleep, 0 when awake
} ps_view_t;

void        ps_init(ps_t *ps, const ps_timing_t *tm, uint32_t now_ms);
void        ps_post(ps_t *ps, ps_event_t ev, uint32_t now_ms);
void        ps_set_ap_stations(ps_t *ps, unsigned n);
// Derives the view. Not const: a change of state is recorded for since_ms and
// counts as activity, so the panel wakes to show it.
ps_view_t   ps_update(ps_t *ps, uint32_t now_ms);
const char *ps_name(ps_state_t st);   // "ready", "offline", ...
