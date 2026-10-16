// SPDX-License-Identifier: GPL-3.0-or-later
// Pure reducer: events and the clock in, one display state out. No FreeRTOS,
// no I/O -- printer_state.c wraps it with a mutex and a poll task, and the host
// tests drive it with a fake clock. Every timer is evaluated lazily in
// ps_update() against the caller's `now`. The clock is uint64 milliseconds
// (esp_timer never wraps in practice): with uint32, every stamp older than
// 49.7 days of uptime would alias back into its window.
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
    uint32_t kick_after_ms;        // idle and not ready this long -> send CTRL-X (W-1039)
    uint32_t kick_every_ms;        // and again at most this often while it stays stuck
} ps_timing_t;

#define PS_TIMING_DEFAULT { 3000, 3000, 30000, 60000, 300000, 5000, 60000 }

// W-1055: a cable test's result, shown over the idle states for a moment.
#define PS_CABLE_PASS    1
#define PS_CABLE_FAIL    2
#define PS_CABLE_MS      1800   // one sweep, left to right and back, 900 ms each way

typedef struct {
    ps_timing_t tm;
    uint64_t    boot_at, line_since, failed_at, activity_at, state_since, kicked_at, cable_at;
    uint8_t     setup, wifi_up, line_ready, job, failed, ap_stations, greeted, kicked;
    uint8_t     job_started_low;   // the line was down when this job began: no printer to wait for
    uint8_t     cable;             // PS_CABLE_*, the last test's result, or 0
    ps_state_t  last;
} ps_t;

typedef struct {
    ps_state_t state;
    uint8_t    phone_joined;  // SETUP only: a station is on the access point
    uint8_t    greeted;       // READY only: entered straight from JOINING or BOOTING,
                              // i.e. Wi-Fi just connected -- the panel's green moment
    uint8_t    asleep;        // READY or OFFLINE only, after sleep_after_ms idle
    uint32_t   since_ms;      // time spent in `state` so far (saturates)
    uint32_t   asleep_ms;     // time since falling asleep, 0 when awake (saturates)
    uint8_t    cable;         // PS_CABLE_* while a cable test's result shows, else 0
    uint32_t   cable_ms;      // time since that result
} ps_view_t;

void        ps_init(ps_t *ps, const ps_timing_t *tm, uint64_t now_ms);
void        ps_post(ps_t *ps, ps_event_t ev, uint64_t now_ms);
void        ps_set_ap_stations(ps_t *ps, unsigned n);
// A cable test finished (W-1055). Its result shows for PS_CABLE_MS over
// Setup, Joining, Ready and Offline; a job hides it.
void        ps_cable(ps_t *ps, int pass, uint64_t now_ms);
// Derives the view. Not const: a change of state is recorded for since_ms and
// counts as activity, so the panel wakes to show it.
ps_view_t   ps_update(ps_t *ps, uint64_t now_ms);
const char *ps_name(ps_state_t st);   // "ready", "offline", ...

// W-1039: noise on the serial line (a cable replug, power-up) can leave the
// ImageWriter stuck busy with SELECT lit -- an unprinted line in its buffer --
// and CTRL-X (cancel current line) clears it. ps_want_kick() says when to send
// one: no job, line not ready for kick_after_ms, at most every kick_every_ms.
// Never mid-job, where a low line is paper or a jam. Call ps_kicked() after
// sending.
int         ps_want_kick(const ps_t *ps, uint64_t now_ms);
void        ps_kicked(ps_t *ps, uint64_t now_ms);
