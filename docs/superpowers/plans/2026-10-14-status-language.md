# Status language v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the AirWriter's single "blocked" state with eight discrete display states, pause stalled jobs instead of aborting them, reconnect Wi-Fi forever, sleep the panel when idle, and play a Spin animation at boot.

**Architecture:** A pure reducer (`printer_state_core`) takes events plus the clock and derives one view `{state, phone_joined, asleep, since_ms, asleep_ms}`; every timer is evaluated lazily against `now`. The LEDs, IPP status and web status all read that one view. `serial_out` stops judging "blocked" and just reports HSKo edges; its pure write loop rides out a stall and fails only after `STALL_ABORT` without progress.

**Tech Stack:** C11, ESP-IDF v5.5.1 (FreeRTOS, esp_wifi, esp_timer, led_strip RMT), host tests built with `cc` under `tools/rip`.

**Spec:** `docs/superpowers/specs/2026-10-14-status-language-design.md` (W-1037). Read it first.

## Global Constraints

- Work only in the worktree at `/private/tmp/claude-501/-Users-wells-Projects-airwriter/62a73e06-37c7-416a-913b-4d30c93e93a8/scratchpad/aw-status` on branch `wells/w-1037-status-language`. Never `git checkout`/`switch` in `/Users/wells/Projects/airwriter`: another session uses that checkout. All paths below are relative to the worktree root.
- Host tests: `make -C tools/rip test` passes, and `make -C tools/rip regress` reports both `stream.bin` regressions unchanged.
- Firmware: `. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build` ends with "Project build complete", with no new warnings.
- Host code builds with `-std=c11 -Wall -Wextra -Werror`.
- New files start with `// SPDX-License-Identifier: GPL-3.0-or-later`.
- Brightness: FR4 dots capped at `LG_BRIGHTNESS_CAP` 48/255; mask dots at `LG_MASK_CAP` 192/255, and only in the all-dot states (Booting, Printing, Needs attention, Job failed).
- Timers: boot 3000 ms, offline debounce 3000 ms, stall attention `CONFIG_AIRWRITER_STALL_TIMEOUT` (30 s), failed hold 60000 ms, sleep 300000 ms, stall abort `CONFIG_AIRWRITER_STALL_ABORT` (600 s).
- IPP reasons: Needs attention → stopped + `media-empty`; Printer offline → stopped + `other` (`offline-report` is not IANA-registered).
- Commit messages end with a `Refs: W-1037` trailer and carry no attribution lines.
- No test patterns ship: no W-1034 rainbow, no boot-animation demo or spike code, no LED test endpoints. W-1034 is not merged into this branch.

## Review Focus

1. HSKo toggles on every printed line. Mid-job, that chatter must never flash Needs attention (30 s of continuous low required) or Printer offline (never shown while a job is active). Pinned in Task 2's core test ("29 s stall is still printing").
2. A job sent to a printer that has been off for more than 10 minutes must wait for it, not fail instantly the way the old latched `BLOCKED` did. Pinned in Task 3's writer test ("a fresh write to a long-dead printer still waits the full limit").
3. Wi-Fi dropping after boot must keep retrying and never fall back to the setup access point. Pinned by the Task 8 router-off check (no host test; this is ESP glue).
4. The ~49.7-day `uint32` millisecond wrap must not break any timer. Pinned in Task 2's core test ("49-day wrap").
5. Home Assistant polls Get-Printer-Attributes every 30 s; that must not keep the panel awake. Pinned by code (`on_job_op` only on Validate-Job/Print-Job, Task 5) and the Task 8 overnight check.

---

### Task 1: Make the worktree buildable

**Files:**
- Create (untracked): `firmware/sdkconfig` (copied)

**Interfaces:** none.

- [ ] **Step 1: Copy the S3 build config into the worktree**

`firmware/sdkconfig` is untracked, so a fresh worktree would build for the wrong chip.

```bash
cd /private/tmp/claude-501/-Users-wells-Projects-airwriter/62a73e06-37c7-416a-913b-4d30c93e93a8/scratchpad/aw-status
cp /Users/wells/Projects/airwriter/firmware/sdkconfig firmware/sdkconfig
grep -E '^CONFIG_IDF_TARGET="esp32s3"' firmware/sdkconfig
```
Expected: the grep prints the target line.

- [ ] **Step 2: Verify host tests and firmware build on the untouched branch**

```bash
make -C tools/rip test && make -C tools/rip regress
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | tail -3
```
Expected: every test prints `ok`, both regressions are unchanged, and the build completes. (The first build in a fresh worktree downloads managed components and takes a few minutes.)

No commit (nothing tracked changes).

---

### Task 2: State core v2, wired in with today's visuals

Replaces the reducer and its glue, and moves every caller to the new events. The LED renderer keeps today's patterns, mapped onto the new state names, until Task 6.

**Files:**
- Rewrite: `firmware/components/printer_state/include/printer_state_core.h`
- Rewrite: `firmware/components/printer_state/printer_state_core.c`
- Rewrite: `firmware/components/printer_state/include/printer_state.h`
- Rewrite: `firmware/components/printer_state/printer_state.c`
- Modify: `firmware/components/printer_state/CMakeLists.txt`
- Modify: `firmware/components/led_grid/led_grid_core.c` (case labels only)
- Modify: `firmware/components/led_grid/led_grid.c` (one call)
- Modify: `firmware/main/main.c` (event calls, status JSON, change callback)
- Rewrite: `tools/rip/test/test_printer_state.c`
- Modify: `tools/rip/test/test_led_grid.c` (state names only)

**Interfaces:**
- Produces (used by every later task):
  - `ps_state_t`: `PS_BOOTING, PS_SETUP, PS_JOINING, PS_ATTENTION, PS_FAILED, PS_PRINTING, PS_OFFLINE, PS_READY`
  - `ps_event_t`: `PS_EV_WIFI_SETUP_AP, PS_EV_WIFI_JOINING, PS_EV_WIFI_UP, PS_EV_WIFI_DOWN, PS_EV_LINE_READY, PS_EV_LINE_NOT_READY, PS_EV_JOB_START, PS_EV_JOB_DONE, PS_EV_JOB_FAILED, PS_EV_ACTIVITY`
  - `ps_timing_t {boot_ms, offline_debounce_ms, stall_attention_ms, failed_hold_ms, sleep_after_ms}`, `PS_TIMING_DEFAULT`
  - `ps_view_t {state, phone_joined, asleep, since_ms, asleep_ms}`
  - Core: `ps_init(ps_t*, const ps_timing_t*, uint32_t now)`, `ps_post(ps_t*, ps_event_t, uint32_t now)`, `ps_set_ap_stations(ps_t*, unsigned)`, `ps_view_t ps_update(ps_t*, uint32_t now)`, `const char *ps_name(ps_state_t)`
  - Glue: `printer_state_start(const ps_timing_t*, void (*on_change)(const ps_view_t*))`, `printer_state_post(ps_event_t)`, `printer_state_set_ap_stations(unsigned)`, `ps_view_t printer_state_view(void)`

- [ ] **Step 1: Write the failing core test**

Replace `tools/rip/test/test_printer_state.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the printer_state reducer: every row of the spec's state
// table, each timer edge, priority between overlapping states, sleep, and the
// uint32 millisecond wrap. docs/superpowers/specs/2026-10-14-status-language-design.md
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "printer_state_core.h"

static const ps_timing_t TM = PS_TIMING_DEFAULT;

// Booted at t0, joining, a ready printer, Wi-Fi up a second later.
static void up_and_ready(ps_t *ps, uint32_t t0)
{
    ps_init(ps, &TM, t0);
    ps_post(ps, PS_EV_WIFI_JOINING, t0);
    ps_post(ps, PS_EV_LINE_READY, t0);
    ps_post(ps, PS_EV_WIFI_UP, t0 + 1000);
}

static ps_state_t st(ps_t *ps, uint32_t t) { return ps_update(ps, t).state; }

int main(void)
{
    ps_t ps;
    ps_view_t v;

    // Booting lasts boot_ms, then the Wi-Fi state shows.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_JOINING, 0);
    assert(st(&ps, 0) == PS_BOOTING);
    assert(st(&ps, 2999) == PS_BOOTING);
    assert(st(&ps, 3000) == PS_JOINING);
    ps_post(&ps, PS_EV_LINE_READY, 3100);
    ps_post(&ps, PS_EV_WIFI_UP, 3200);
    assert(st(&ps, 3200) == PS_READY);

    // Idle and not ready: still ready inside the debounce, then offline; back on, ready.
    up_and_ready(&ps, 0);
    st(&ps, 4000);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
    assert(st(&ps, 12999) == PS_READY);
    assert(st(&ps, 13000) == PS_OFFLINE);
    ps_post(&ps, PS_EV_LINE_READY, 20000);
    assert(st(&ps, 20000) == PS_READY);

    // A job. HSKo chatters on every line, so a 29 s stall is still printing;
    // 30 s asks for attention; the line returning resumes; done is ready.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    assert(st(&ps, 5000) == PS_PRINTING);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 10000);
    assert(st(&ps, 39999) == PS_PRINTING);
    assert(st(&ps, 40000) == PS_ATTENTION);
    ps_post(&ps, PS_EV_LINE_READY, 50000);
    assert(st(&ps, 50000) == PS_PRINTING);
    ps_post(&ps, PS_EV_JOB_DONE, 60000);
    assert(st(&ps, 60000) == PS_READY);

    // The printer still busy for a moment after the last byte is not offline.
    ps_post(&ps, PS_EV_LINE_NOT_READY, 60100);
    assert(st(&ps, 62100) == PS_READY);
    ps_post(&ps, PS_EV_LINE_READY, 62200);
    assert(st(&ps, 70000) == PS_READY);

    // A job sent to a printer that has been off a while asks for attention at once.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 5000);
    assert(st(&ps, 45000) == PS_OFFLINE);
    ps_post(&ps, PS_EV_JOB_START, 45000);
    assert(st(&ps, 45000) == PS_ATTENTION);

    // A failed job holds for failed_hold_ms; the next job clears it at once.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    ps_post(&ps, PS_EV_JOB_FAILED, 6000);
    assert(st(&ps, 6000) == PS_FAILED);
    assert(st(&ps, 65999) == PS_FAILED);
    assert(st(&ps, 66000) == PS_READY);
    ps_post(&ps, PS_EV_JOB_START, 70000);
    ps_post(&ps, PS_EV_JOB_FAILED, 71000);
    ps_post(&ps, PS_EV_JOB_START, 72000);
    assert(st(&ps, 72000) == PS_PRINTING);

    // Wi-Fi lost mid-job shows joining; a failure while down shows once back up.
    up_and_ready(&ps, 0);
    ps_post(&ps, PS_EV_JOB_START, 5000);
    ps_post(&ps, PS_EV_WIFI_DOWN, 6000);
    assert(st(&ps, 6000) == PS_JOINING);
    ps_post(&ps, PS_EV_JOB_FAILED, 7000);
    ps_post(&ps, PS_EV_WIFI_UP, 9000);
    assert(st(&ps, 9000) == PS_FAILED);

    // Setup outranks everything below booting; phone_joined follows the station count.
    ps_init(&ps, &TM, 0);
    ps_post(&ps, PS_EV_WIFI_SETUP_AP, 0);
    v = ps_update(&ps, 5000);
    assert(v.state == PS_SETUP && !v.phone_joined);
    ps_set_ap_stations(&ps, 1);
    assert(ps_update(&ps, 5100).phone_joined);
    ps_set_ap_stations(&ps, 0);
    assert(!ps_update(&ps, 5200).phone_joined);

    // since_ms counts from entry into the state.
    up_and_ready(&ps, 0);
    st(&ps, 4000);                                  // READY from 4000
    assert(ps_update(&ps, 4500).since_ms == 500);

    // Sleep: ready with no activity sleeps after sleep_after_ms; activity wakes
    // it; a change of state wakes it; offline sleeps too; printing never does.
    up_and_ready(&ps, 0);
    st(&ps, 4000);
    assert(!ps_update(&ps, 303999).asleep);
    v = ps_update(&ps, 304000);
    assert(v.asleep && v.asleep_ms == 0);
    assert(ps_update(&ps, 314000).asleep_ms == 10000);
    ps_post(&ps, PS_EV_ACTIVITY, 320000);
    assert(!ps_update(&ps, 320000).asleep);
    ps_post(&ps, PS_EV_LINE_NOT_READY, 330000);
    assert(st(&ps, 333000) == PS_OFFLINE);
    assert(!ps_update(&ps, 632999).asleep);
    assert(ps_update(&ps, 633000).asleep);
    ps_post(&ps, PS_EV_LINE_READY, 640000);
    ps_post(&ps, PS_EV_JOB_START, 640000);
    assert(!ps_update(&ps, 1040000).asleep);

    // The 49-day wrap: timers measured across zero still work.
    {
        uint32_t t0 = 0xFFFFFFFFu - 1000u;
        up_and_ready(&ps, t0);
        st(&ps, t0 + 4000u);
        ps_post(&ps, PS_EV_LINE_NOT_READY, t0 + 5000u);
        assert(st(&ps, t0 + 7999u) == PS_READY);
        assert(st(&ps, t0 + 8000u) == PS_OFFLINE);
    }

    assert(strcmp(ps_name(PS_READY), "ready") == 0);
    assert(strcmp(ps_name(PS_OFFLINE), "offline") == 0);
    assert(strcmp(ps_name(PS_ATTENTION), "attention") == 0);

    printf("test_printer_state: ok\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make -C tools/rip build/test_printer_state`
Expected: compile errors, such as `'PS_BOOTING' undeclared` and `too many arguments to function 'ps_init'`.

- [ ] **Step 3: Write the core header**

Replace `firmware/components/printer_state/include/printer_state_core.h` with:

```c
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
    uint8_t     setup, wifi_up, line_ready, job, failed, ap_stations;
    ps_state_t  last;
} ps_t;

typedef struct {
    ps_state_t state;
    uint8_t    phone_joined;  // SETUP only: a station is on the access point
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
```

- [ ] **Step 4: Write the core**

Replace `firmware/components/printer_state/printer_state_core.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Device-state reducer. Pure C on purpose -- the ESP glue and the host tests
// share this file. Priority does the work; there is no transition table.
#include "printer_state_core.h"
#include <string.h>

void ps_init(ps_t *ps, const ps_timing_t *tm, uint32_t now)
{
    memset(ps, 0, sizeof *ps);
    ps->tm = *tm;
    ps->boot_at = ps->line_since = ps->activity_at = ps->state_since = now;
    ps->last = PS_BOOTING;
}

void ps_post(ps_t *ps, ps_event_t ev, uint32_t now)
{
    switch (ev) {
    case PS_EV_WIFI_SETUP_AP:  ps->setup = 1;   break;
    case PS_EV_WIFI_JOINING:
    case PS_EV_WIFI_DOWN:      ps->wifi_up = 0; break;
    case PS_EV_WIFI_UP:        ps->wifi_up = 1; break;
    case PS_EV_LINE_READY:
        if (!ps->line_ready) { ps->line_ready = 1; ps->line_since = now; }
        break;
    case PS_EV_LINE_NOT_READY:
        if (ps->line_ready) { ps->line_ready = 0; ps->line_since = now; }
        break;
    case PS_EV_JOB_START:  ps->job = 1; ps->failed = 0; ps->activity_at = now; break;
    case PS_EV_JOB_DONE:   ps->job = 0; break;
    case PS_EV_JOB_FAILED: ps->job = 0; ps->failed = 1; ps->failed_at = now; break;
    case PS_EV_ACTIVITY:   ps->activity_at = now; break;
    }
}

void ps_set_ap_stations(ps_t *ps, unsigned n)
{
    ps->ap_stations = (uint8_t)(n > 255 ? 255 : n);
}

static ps_state_t derive(const ps_t *ps, uint32_t now)
{
    if (now - ps->boot_at < ps->tm.boot_ms) return PS_BOOTING;
    if (ps->setup)    return PS_SETUP;
    if (!ps->wifi_up) return PS_JOINING;
    uint32_t low_for = ps->line_ready ? 0 : now - ps->line_since;
    if (ps->job && !ps->line_ready && low_for >= ps->tm.stall_attention_ms)
        return PS_ATTENTION;
    if (ps->failed && now - ps->failed_at < ps->tm.failed_hold_ms) return PS_FAILED;
    if (ps->job) return PS_PRINTING;
    if (!ps->line_ready && low_for >= ps->tm.offline_debounce_ms) return PS_OFFLINE;
    return PS_READY;
}

ps_view_t ps_update(ps_t *ps, uint32_t now)
{
    ps_state_t st = derive(ps, now);
    if (st != ps->last) {
        ps->last = st;
        ps->state_since = now;
        ps->activity_at = now;
    }
    ps_view_t v = { .state = st, .since_ms = now - ps->state_since };
    v.phone_joined = st == PS_SETUP && ps->ap_stations > 0;
    if ((st == PS_READY || st == PS_OFFLINE) &&
        now - ps->activity_at >= ps->tm.sleep_after_ms) {
        v.asleep    = 1;
        v.asleep_ms = now - ps->activity_at - ps->tm.sleep_after_ms;
    }
    return v;
}

const char *ps_name(ps_state_t st)
{
    switch (st) {
    case PS_BOOTING:   return "booting";
    case PS_SETUP:     return "setup";
    case PS_JOINING:   return "joining";
    case PS_ATTENTION: return "attention";
    case PS_FAILED:    return "failed";
    case PS_PRINTING:  return "printing";
    case PS_OFFLINE:   return "offline";
    case PS_READY:     return "ready";
    }
    return "?";
}
```

- [ ] **Step 5: Run the core test to verify it passes**

Run: `make -C tools/rip build/test_printer_state && ./tools/rip/build/test_printer_state`
Expected: `test_printer_state: ok`

- [ ] **Step 6: Rewrite the ESP glue**

Replace `firmware/components/printer_state/include/printer_state.h` with:

```c
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
ps_view_t printer_state_view(void);                    // any task
```

Replace `firmware/components/printer_state/printer_state.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue: a mutex-guarded ps_t on the esp_timer clock, plus the poll task.
#include "printer_state.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static ps_t s_ps;
static SemaphoreHandle_t s_mtx;
static void (*s_cb)(const ps_view_t *);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void poll_task(void *arg)
{
    (void)arg;
    int told = -1;
    for (;;) {
        ps_view_t v = printer_state_view();
        if ((int)v.state != told) {
            told = (int)v.state;
            if (s_cb) s_cb(&v);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void printer_state_start(const ps_timing_t *tm, void (*on_change)(const ps_view_t *))
{
    s_mtx = xSemaphoreCreateMutex();
    ps_init(&s_ps, tm, now_ms());
    s_cb = on_change;
    xTaskCreate(poll_task, "ps_poll", 3072, NULL, 4, NULL);
}

void printer_state_post(ps_event_t ev)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_post(&s_ps, ev, now_ms());
    xSemaphoreGive(s_mtx);
}

void printer_state_set_ap_stations(unsigned n)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_set_ap_stations(&s_ps, n);
    xSemaphoreGive(s_mtx);
}

ps_view_t printer_state_view(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    ps_view_t v = ps_update(&s_ps, now_ms());
    xSemaphoreGive(s_mtx);
    return v;
}
```

In `firmware/components/printer_state/CMakeLists.txt`, replace:
```
idf_component_register(SRCS "printer_state.c" "printer_state_core.c"
                       INCLUDE_DIRS "include")
```
with:
```
idf_component_register(SRCS "printer_state.c" "printer_state_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_timer)
```

- [ ] **Step 7: Map the LED renderer onto the new names (visuals unchanged until Task 6)**

In `firmware/components/led_grid/led_grid_core.c`, make exactly these replacements:
- `    case PS_BOOT:` → `    case PS_BOOTING:`
- `    case PS_WIFI_CONNECTING:` → `    case PS_JOINING:`
- `    case PS_BLOCKED: {` → `    case PS_ATTENTION:\n    case PS_OFFLINE: {`
- `    case PS_ERROR: {` → `    case PS_FAILED: {`
- `            if (st == PS_ERROR) v = 0;` → `            if (st == PS_FAILED) v = 0;`

In `firmware/components/led_grid/led_grid.c`, replace `lg_render(printer_state_get(),` with `lg_render(printer_state_view().state,`.

In `tools/rip/test/test_led_grid.c`, replace the state list
```c
    ps_state_t all[] = { PS_BOOT, PS_SETUP, PS_WIFI_CONNECTING, PS_READY,
                         PS_PRINTING, PS_BLOCKED, PS_ERROR };
```
with
```c
    ps_state_t all[] = { PS_BOOTING, PS_SETUP, PS_JOINING, PS_READY,
                         PS_PRINTING, PS_ATTENTION, PS_FAILED, PS_OFFLINE };
```
then replace every remaining `PS_BOOT,`/`PS_BOOT)` with `PS_BOOTING,`/`PS_BOOTING)` and every `PS_ERROR` with `PS_FAILED`.

- [ ] **Step 8: Move `main.c` to the new API**

In `firmware/main/main.c`:

(a) Replace the whole `printer_state_changed` function (from its leading comment through its closing brace) with the interim bridge below. Task 3 replaces it again.
```c
// Interim (Task 3 replaces it): serial_out's 30 s BLOCKED verdict stands in
// for the line level until serial_out reports HSKo edges itself.
static void printer_state_changed(void *ctx, serial_out_state_t st)
{
    (void)ctx;
    printer_state_post(st == SERIAL_OUT_BLOCKED ? PS_EV_LINE_NOT_READY
                                                 : PS_EV_LINE_READY);
}
```

(b) Rename the event calls:
- `printer_state_post_ev(PS_EV_JOB_START);` → `printer_state_post(PS_EV_JOB_START);` (both occurrences: `doc_begin` and `test_page_task`)
- `printer_state_post_ev(PS_EV_JOB_FAIL);` → `printer_state_post(PS_EV_JOB_FAILED);`
- `printer_state_post_ev(PS_EV_JOB_DONE);` → `printer_state_post(PS_EV_JOB_DONE);`
- `printer_state_post_ev(s_printer_failed ? PS_EV_JOB_FAIL : PS_EV_JOB_DONE);` → `printer_state_post(s_printer_failed ? PS_EV_JOB_FAILED : PS_EV_JOB_DONE);`
- `printer_state_post_ev(PS_EV_WIFI_UP);` → `printer_state_post(PS_EV_WIFI_UP);`
- `printer_state_post_ev(PS_EV_WIFI_CONNECTING);` → `printer_state_post(PS_EV_WIFI_JOINING);`
- `printer_state_post_ev(PS_EV_SETUP_MODE);` → `printer_state_post(PS_EV_WIFI_SETUP_AP);`

(c) In `status_json`, replace
```c
    int blocked = serial_out_state() == SERIAL_OUT_BLOCKED;
```
with
```c
    ps_view_t v = printer_state_view();
    int blocked = v.state == PS_ATTENTION || v.state == PS_OFFLINE;
```
and replace `ps_name(printer_state_get()),` with `ps_name(v.state),`.

(d) Replace the `on_ps_change` function with:
```c
// Runs on printer_state's poll task, once per change of display state.
static void on_ps_change(const ps_view_t *v)
{
    ESP_LOGI(TAG, "state -> %s", ps_name(v->state));
    // SPEC.md 3.9: a stall mid-job becomes a real alert on the phone.
    ipp_server_set_stopped(v->state == PS_ATTENTION ? "media-empty" : NULL);
}
```

(e) In `app_main`, replace `    printer_state_start(on_ps_change);` with:
```c
    ps_timing_t tm = PS_TIMING_DEFAULT;
    tm.stall_attention_ms = CONFIG_AIRWRITER_STALL_TIMEOUT * 1000u;
    printer_state_start(&tm, on_ps_change);
```

(f) Directly after `    ESP_ERROR_CHECK(serial_out_init(&ser));`, add the interim initial level (Task 3 removes it):
```c
    printer_state_post(serial_out_clear() ? PS_EV_LINE_READY : PS_EV_LINE_NOT_READY);
```

- [ ] **Step 9: Verify no old names remain, then test and build**

```bash
grep -rn -E "printer_state_post_ev|printer_state_get|PS_EV_PRINTER_|PS_BLOCKED|PS_ERROR|PS_BOOT\b|PS_WIFI_CONNECTING|PS_EV_SETUP_MODE|PS_EV_WIFI_CONNECTING|PS_EV_JOB_FAIL\b" firmware tools/rip
make -C tools/rip test && make -C tools/rip regress
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -E "error|warning: |complete"
```
Expected: the grep prints nothing, every test passes, both regressions are unchanged, and "Project build complete".

- [ ] **Step 10: Commit**

```bash
git add firmware/components/printer_state firmware/components/led_grid/led_grid_core.c firmware/components/led_grid/led_grid.c firmware/main/main.c tools/rip/test/test_printer_state.c tools/rip/test/test_led_grid.c
git commit -m "printer_state: discrete display states on a lazy clock

Eight states in priority order (booting, setup, joining, attention,
failed, printing, offline, ready) derived from events plus now, with an
asleep modifier. Every timer is evaluated in ps_update(), so the core
stays pure and the host test drives it with a fake clock. The LEDs keep
today's patterns, mapped onto the new names, until the renderer is
rewritten.

Refs: W-1037"
```

---

### Task 3: Serial reports HSKo edges, and a stalled job pauses instead of aborting

**Files:**
- Rewrite: `firmware/components/serial_out/include/serial_writer.h`
- Rewrite: `firmware/components/serial_out/serial_writer.c`
- Rewrite: `tools/rip/test/test_serial_writer.c`
- Modify: `firmware/components/serial_out/include/serial_out.h`
- Modify: `firmware/components/serial_out/serial_out.c`
- Modify: `firmware/main/Kconfig.projbuild`
- Modify: `firmware/components/job_pipe/job_pipe.c`
- Modify: `firmware/main/main.c`

**Interfaces:**
- Consumes: `printer_state_post(PS_EV_LINE_READY | PS_EV_LINE_NOT_READY)` from Task 2.
- Produces: `serial_writer_ops_t { ctx, tx_write, delay, stall_limit }`; `serial_out_cfg_t` gains `stall_abort_s`, `on_line(void *ctx, int ready)` and `on_line_ctx`, and loses `stall_timeout_s`, `on_state` and `on_state_ctx`. `serial_out_state()` and `serial_out_state_t` are removed. Kconfig gains `AIRWRITER_STALL_ABORT` (default 600).

- [ ] **Step 1: Write the failing writer test**

Replace `tools/rip/test/test_serial_writer.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serial_out write loop, off the hardware. The fake models what
// uart_tx_chars really gives us: a hardware FIFO that accepts what fits and
// reports the count. The printer can pause (drain_after) and resume. The loop
// must ride out any pause shorter than stall_limit delays without progress,
// and give up only after that -- a paused job (paper out, SELECT) resumes.
#include <stdlib.h>
#include "test_util.h"
#include "serial_writer.h"

#define FAKE_FIFO 8

typedef struct {
    uint8_t fifo_taken[4096];  // every byte the "FIFO" accepted, in order
    size_t  taken;
    size_t  free;              // FIFO space right now
    size_t  drain_per_delay;   // bytes the "printer" absorbs each delay period
    int     drain_after;       // delay periods before the printer starts draining
    int     delays;
    int     writes;
    int     write_err;         // force tx_write to fail
} fake_t;

static int f_tx_write(void *ctx, const uint8_t *buf, size_t n)
{
    fake_t *f = (fake_t *)ctx;
    f->writes++;
    if (f->write_err) return -1;
    size_t take = n < f->free ? n : f->free;
    memcpy(f->fifo_taken + f->taken, buf, take);
    f->taken += take;
    f->free  -= take;
    return (int)take;
}

static void f_delay(void *ctx)
{
    fake_t *f = (fake_t *)ctx;
    if (f->delays > 100000) {
        fprintf(stderr, "FAIL fake: writer is spinning forever\n");
        exit(1);
    }
    f->delays++;
    if (f->delays > f->drain_after) f->free += f->drain_per_delay;
    if (f->free > FAKE_FIFO) f->free = FAKE_FIFO;
}

static serial_writer_ops_t ops_for(fake_t *f, unsigned stall_limit)
{
    serial_writer_ops_t ops = {
        .ctx         = f,
        .tx_write    = f_tx_write,
        .delay       = f_delay,
        .stall_limit = stall_limit,
    };
    return ops;
}

int main(void)
{
    // A payload that fits the FIFO goes through in one write, no delays.
    {
        fake_t f = { .free = FAKE_FIFO };
        serial_writer_ops_t ops = ops_for(&f, 5);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
        CHECK_EQ_INT(f.taken, 5);
        CHECK_EQ_MEM(f.fifo_taken, msg, 5);
        CHECK_EQ_INT(f.writes, 1);
        CHECK_EQ_INT(f.delays, 0);
    }

    // A payload bigger than the FIFO drains through in order, paced.
    {
        fake_t f = { .free = FAKE_FIFO, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f, 5);
        uint8_t msg[20];
        for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), 0);
        CHECK_EQ_INT(f.taken, sizeof msg);
        CHECK_EQ_MEM(f.fifo_taken, msg, sizeof msg);
        CHECK(f.delays > 0);
        CHECK(f.writes <= f.delays + 1);
    }

    // A full FIFO that never drains: poll stall_limit times, then give up.
    {
        fake_t f = { .free = 0, .drain_after = 1 << 30 };
        serial_writer_ops_t ops = ops_for(&f, 3);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.taken, 0);
        CHECK_EQ_INT(f.delays, 3);
        CHECK_EQ_INT(f.writes, 4);
    }

    // A fresh write to a printer that has been dead a long time still waits
    // the full limit: nothing is remembered across calls, so a job sent while
    // the printer is off gets its whole pause, not an instant failure.
    {
        fake_t f = { .free = 0, .drain_after = 1 << 30 };
        serial_writer_ops_t ops = ops_for(&f, 3);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        f.delays = 0;
        f.writes = 0;
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.delays, 3);
    }

    // The printer dies mid-stream: the delay after real progress does not
    // count; then stall_limit delays with nothing taken, then give up.
    {
        fake_t f = { .free = 4, .drain_after = 1 << 30 };
        serial_writer_ops_t ops = ops_for(&f, 2);
        uint8_t msg[20] = {0};
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), -1);
        CHECK_EQ_INT(f.taken, 4);
        CHECK_EQ_INT(f.delays, 3);
    }

    // Paper out and back: a 50-period pause inside a 100-period limit resumes
    // and delivers every byte in order.
    {
        fake_t f = { .free = 0, .drain_after = 50, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f, 100);
        uint8_t msg[20];
        for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(0xA0 + i);
        CHECK_EQ_INT(serial_writer_write(&ops, msg, sizeof msg), 0);
        CHECK_EQ_INT(f.taken, sizeof msg);
        CHECK_EQ_MEM(f.fifo_taken, msg, sizeof msg);
        CHECK(f.delays > 50);
    }

    // A pause longer than the limit fails.
    {
        fake_t f = { .free = 0, .drain_after = 50, .drain_per_delay = 4 };
        serial_writer_ops_t ops = ops_for(&f, 10);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.delays, 10);
    }

    // A slow trickle (head returns) never trips the limit: any progress resets it.
    {
        fake_t f = { .free = 0, .drain_per_delay = 1 };
        serial_writer_ops_t ops = ops_for(&f, 1);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
        CHECK_EQ_INT(f.taken, 5);
    }

    // stall_limit 0 means never give up (with a printer that does come back).
    {
        fake_t f = { .free = 0, .drain_after = 500, .drain_per_delay = 8 };
        serial_writer_ops_t ops = ops_for(&f, 0);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), 0);
    }

    // Driver failure is fatal, not retried.
    {
        fake_t f = { .free = FAKE_FIFO, .write_err = 1 };
        serial_writer_ops_t ops = ops_for(&f, 5);
        const uint8_t msg[] = "hello";
        CHECK_EQ_INT(serial_writer_write(&ops, msg, 5), -1);
        CHECK_EQ_INT(f.writes, 1);
    }

    // Nothing to send is success and touches nothing.
    {
        fake_t f = { .free = 0 };
        serial_writer_ops_t ops = ops_for(&f, 5);
        CHECK_EQ_INT(serial_writer_write(&ops, NULL, 0), 0);
        CHECK_EQ_INT(f.writes, 0);
        CHECK_EQ_INT(f.delays, 0);
    }

    TEST_END;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make -C tools/rip build/test_serial_writer`
Expected: a compile error, `'serial_writer_ops_t' has no member named 'stall_limit'`.

- [ ] **Step 3: Rewrite the writer**

Replace `firmware/components/serial_out/include/serial_writer.h` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The write loop of serial_out_write(), pure C so it runs under the host
// tests in tools/rip. serial_out.c binds these ops to the UART driver; the
// tests bind them to a fake.
//
// Every op is non-blocking: tx_write takes what fits in the hardware FIFO and
// reports the count, and that return value is ground truth. A printer that
// holds HSKo false (paper out, SELECT, powered off) simply stops the FIFO
// draining; the loop waits it out and gives up only after stall_limit delays
// in a row with nothing accepted. Nothing is remembered across calls, so every
// write -- including the first one to a printer that has been off for an
// hour -- gets the whole limit.
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    void *ctx;
    // Queue up to n bytes into the hardware FIFO without blocking. Returns
    // bytes accepted (0 when the FIFO is full), or negative on driver error.
    int (*tx_write)(void *ctx, const uint8_t *buf, size_t n);
    // Sleep one poll period.
    void (*delay)(void *ctx);
    // Consecutive delays with nothing accepted before the write gives up.
    // 0 = never.
    unsigned stall_limit;
} serial_writer_ops_t;

// 0 with every byte queued, or -1 with bytes abandoned because the FIFO took
// nothing for stall_limit delays in a row, or the driver failed. Paced: any
// call that leaves a remainder is followed by one delay, never a hot retry.
int serial_writer_write(const serial_writer_ops_t *ops, const uint8_t *buf, size_t n);
```

Replace `firmware/components/serial_out/serial_writer.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "serial_writer.h"

int serial_writer_write(const serial_writer_ops_t *ops, const uint8_t *buf, size_t n)
{
    unsigned idle = 0;
    while (n) {
        int w = ops->tx_write(ops->ctx, buf, n);
        if (w < 0) return -1;
        buf += w;
        n   -= (size_t)w;
        if (!n) break;
        if (w > 0) idle = 0;
        else if (ops->stall_limit && ++idle > ops->stall_limit) return -1;
        ops->delay(ops->ctx);
    }
    return 0;
}
```

- [ ] **Step 4: Run the writer test to verify it passes**

Run: `make -C tools/rip build/test_serial_writer && ./tools/rip/build/test_serial_writer`
Expected: `ok  test/test_serial_writer.c  (N checks)` with no FAIL lines.

- [ ] **Step 5: Make `serial_out` report edges and use the stall limit**

In `firmware/components/serial_out/include/serial_out.h`:

(a) Replace the header comment's second paragraph ("That same line is the paper sensor. … so the phone shows a real alert.") with:
```c
// That same line is the printer's only status. serial_out reports its edges
// and nothing more; printer_state decides what a low line means (offline when
// idle, needs attention mid-job). A write waits out a low line -- a paused job
// resumes when paper returns -- and fails only after stall_abort_s with no
// progress.
```

(b) Delete the whole `typedef enum { SERIAL_OUT_READY, … } serial_out_state_t;` block.

(c) Replace this block in `serial_out_cfg_t`:
```c
    // Seconds of continuous HSKo deassertion before the printer is called
    // blocked. 0 uses the default of 30.
    int      stall_timeout_s;

    // Reports a state change. Called from the watchdog task.
    void   (*on_state)(void *ctx, serial_out_state_t st);
    void    *on_state_ctx;
```
with:
```c
    // Seconds a write waits with nothing accepted before failing the job.
    // 0 uses the default of 600.
    int      stall_abort_s;

    // Reports HSKo edges (1 = ready), sampled every 100 ms, plus the level at
    // start. Called from serial_out's line task. With loopback on it reports
    // ready once (bytes do flow); with no CTS pin it reports not-ready once.
    void   (*on_line)(void *ctx, int ready);
    void    *on_line_ctx;
```

(d) Replace the `serial_out_write` comment with:
```c
// Blocks until every byte is queued. With CTS flow control that means it
// waits while the printer is busy or paused, which is the backpressure the
// raster pipeline wants -- but not past stall_abort_s with no progress, when
// it returns ESP_FAIL with the rest of the payload abandoned. The caller owns
// failing the job.
```

(e) Delete the line `serial_out_state_t serial_out_state(void);`.

In `firmware/components/serial_out/serial_out.c`:

(a) Replace `static volatile serial_out_state_t s_state = SERIAL_OUT_READY;` with `static unsigned s_stall_limit;   // poll periods; see serial_writer.h`.

(b) Replace the whole `watchdog_task` function, including its two-line leading comment ("The printer stalls constantly and briefly …"), with:
```c
// Reports HSKo edges. Mid-job the line toggles on every printed line, so the
// callback must stay cheap; printer_state does the judging on its own clock.
static void line_task(void *arg)
{
    (void)arg;
    int last = -1;
    for (;;) {
        int now = clear_to_send();
        if (now != last) {
            last = now;
            ESP_LOGD(TAG, "printer %s", now ? "ready" : "not ready");
            if (s_cfg.on_line) s_cfg.on_line(s_cfg.on_line_ctx, now);
        }
        vTaskDelay(WATCH_PERIOD);
    }
}
```

(c) In `serial_out_init`, just after `if (!s_cfg.baud) s_cfg.baud = 9600;`, add:
```c
    s_stall_limit = (unsigned)(s_cfg.stall_abort_s > 0 ? s_cfg.stall_abort_s : 600)
                    * (1000u / 100u);   // WATCH_PERIOD is 100 ms
```

(d) In `serial_out_init`, replace
```c
    } else if (s_cfg.cts_pin >= 0) {
        xTaskCreate(watchdog_task, "ser_wd", 3072, NULL, 3, NULL);
    }
```
with
```c
    } else if (s_cfg.cts_pin >= 0) {
        xTaskCreate(line_task, "ser_line", 3072, NULL, 3, NULL);
    }
    // No line to watch: say so once, so the state is not left unknown.
    if (s_cfg.loopback && s_cfg.on_line)      s_cfg.on_line(s_cfg.on_line_ctx, 1);
    else if (s_cfg.cts_pin < 0 && s_cfg.on_line) s_cfg.on_line(s_cfg.on_line_ctx, 0);
```

(e) Delete the whole `ops_blocked` function.

(f) Replace the `serial_out_write` function with:
```c
esp_err_t serial_out_write(const uint8_t *buf, size_t n)
{
    const serial_writer_ops_t ops = {
        .tx_write    = ops_tx_write,
        .delay       = ops_delay,
        .stall_limit = s_stall_limit,
    };
    return serial_writer_write(&ops, buf, n) == 0 ? ESP_OK : ESP_FAIL;
}
```

(g) Delete the line `serial_out_state_t serial_out_state(void) { return s_state; }`.

- [ ] **Step 6: Add the Kconfig and stretch the pipeline's patience**

In `firmware/main/Kconfig.projbuild`, replace
```
config AIRWRITER_STALL_TIMEOUT
    int "Seconds of HSKo deassertion before the printer is called out of paper"
    default 30
```
with
```
config AIRWRITER_STALL_TIMEOUT
    int "Seconds of HSKo deassertion mid-job before the panel asks for attention"
    default 30
    help
        The printer holds HSKo false briefly on every line; only a stall this
        long mid-job means paper, a jam or SELECT. The job pauses, it is not
        failed -- see AIRWRITER_STALL_ABORT.

config AIRWRITER_STALL_ABORT
    int "Seconds a paused job waits for the printer before it is failed"
    default 600
```

In `firmware/components/job_pipe/job_pipe.c`:
- After `#include "job_pipe.h"`, add `#include "sdkconfig.h"`.
- Replace `#define PUSH_TIMEOUT pdMS_TO_TICKS(20000)` with:
```c
// A paused job (paper out, SELECT) backs the upload up behind the printer, so
// the push waits as long as the serial writer will, plus a margin.
#define PUSH_SECONDS (CONFIG_AIRWRITER_STALL_ABORT + 30)
#define PUSH_TIMEOUT pdMS_TO_TICKS(PUSH_SECONDS * 1000)
```
- Replace `ESP_LOGE(TAG, "pipeline stalled for 20s; abandoning the job");` with `ESP_LOGE(TAG, "pipeline stalled for %ds; abandoning the job", PUSH_SECONDS);`
- Replace `if (waited >= 600) { ESP_LOGE(TAG, "pipeline stuck for 10 minutes"); break; }` with `if (waited >= CONFIG_AIRWRITER_STALL_ABORT + 600) { ESP_LOGE(TAG, "pipeline stuck past the stall limit"); break; }`

- [ ] **Step 7: Wire `main.c` to the line callback**

In `firmware/main/main.c`:

(a) Replace the interim `printer_state_changed` function from Task 2 with:
```c
// HSKo edges from serial_out. printer_state turns a low line into "printer
// offline" (idle) or "needs attention" (mid-job) on its own clock.
static void printer_line_changed(void *ctx, int ready)
{
    (void)ctx;
    printer_state_post(ready ? PS_EV_LINE_READY : PS_EV_LINE_NOT_READY);
}
```

(b) In the `serial_out_cfg_t ser = { … }` initializer, replace
```c
        .stall_timeout_s = CONFIG_AIRWRITER_STALL_TIMEOUT,
        .on_state        = printer_state_changed,
```
with
```c
        .stall_abort_s   = CONFIG_AIRWRITER_STALL_ABORT,
        .on_line         = printer_line_changed,
```

(c) Delete Task 2's interim line `    printer_state_post(serial_out_clear() ? PS_EV_LINE_READY : PS_EV_LINE_NOT_READY);`

(d) In the comment above `printer_out`, replace "A write failure means the watchdog called the printer gone; abort the job so the rip task winds down instead of pushing the rest of the page at a dead line." with "A write failure means the printer stayed paused past STALL_ABORT; abort the job so the rip task winds down instead of pushing the rest of the page at a dead line."

- [ ] **Step 8: Verify**

```bash
grep -rn -E "serial_out_state|SERIAL_OUT_BLOCKED|SERIAL_OUT_READY|stall_timeout_s|on_state\b|ops->blocked|\.blocked" firmware tools/rip
make -C tools/rip test && make -C tools/rip regress
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -E "error|warning: |complete"
grep -E "^CONFIG_AIRWRITER_STALL_ABORT=600" firmware/sdkconfig
```
Expected: the first grep prints nothing, tests and regressions pass, the build completes, and the last grep prints the line (the build writes the new default into `sdkconfig`).

- [ ] **Step 9: Commit**

```bash
git add firmware/components/serial_out tools/rip/test/test_serial_writer.c firmware/main/Kconfig.projbuild firmware/components/job_pipe/job_pipe.c firmware/main/main.c
git commit -m "serial_out: report HSKo edges; a stalled job pauses instead of aborting

serial_out no longer decides that a low line means paper out. It reports
edges, and printer_state judges them in context. The write loop rides
out a stall and gives up only after AIRWRITER_STALL_ABORT (600 s) with
nothing accepted. Nothing is latched across writes, so a job to a
printer that has been off a long time still gets its whole wait.
job_pipe's push and end waits stretch to match.

Refs: W-1037"
```

---

### Task 4: Wi-Fi reconnects forever; setup AP counts phones

ESP glue only, so there is no host test. Verified on the board in Task 8.

**Files:**
- Modify: `firmware/main/main.c`

**Interfaces:**
- Consumes: `printer_state_post(PS_EV_WIFI_DOWN | PS_EV_WIFI_UP)`, `printer_state_set_ap_stations(unsigned)` from Task 2.

- [ ] **Step 1: Add the reconnect state and timer callback**

In `firmware/main/main.c`, directly after the line `static int  s_ap_mode;`, add:
```c

// Reconnect after a drop. Once the board has had the network it never falls
// back to setup mode on its own -- a router reboot is no reason to make the
// user re-provision -- it keeps trying, backing off 1, 2, 4 ... 30 s.
static int s_wifi_was_up;
static uint32_t s_backoff_ms = 1000;
static esp_timer_handle_t s_reconnect;

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}
```

- [ ] **Step 2: Reconnect on disconnect after having been up**

In `wifi_event`, replace the `WIFI_EVENT_STA_DISCONNECTED` branch:
```c
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_retries < WIFI_MAX_RETRY) {
```
with:
```c
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_wifi_was_up) {
            printer_state_post(PS_EV_WIFI_DOWN);
            strncpy(s_ip, "0.0.0.0", sizeof s_ip - 1);
            ESP_LOGW(TAG, "wifi lost (reason %d); retrying in %us",
                     d ? d->reason : -1, (unsigned)(s_backoff_ms / 1000));
            esp_timer_start_once(s_reconnect, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms = s_backoff_ms >= 15000 ? 30000 : s_backoff_ms * 2;
        } else if (s_retries < WIFI_MAX_RETRY) {
```
(The existing `s_retries` branch and its `else { xEventGroupSetBits(...) }` stay as they are.)

In the `IP_EVENT_STA_GOT_IP` branch, directly after `s_retries = 0;`, add:
```c
        s_wifi_was_up = 1;
        s_backoff_ms  = 1000;
```

- [ ] **Step 3: Create the reconnect timer in `wifi_init`**

In `wifi_init`, directly after `ESP_ERROR_CHECK(esp_wifi_init(&ic));`, add:
```c

    const esp_timer_create_args_t ta = { .callback = reconnect_cb, .name = "wifi_rejoin" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_reconnect));
```

- [ ] **Step 4: Count stations on the setup access point**

Directly above `static esp_err_t wifi_start(void)`'s leading comment, add:
```c
// Setup mode: count phones on the access point, for the solid-blue cue.
static void ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    static unsigned n;
    if (id == WIFI_EVENT_AP_STACONNECTED) n++;
    else if (id == WIFI_EVENT_AP_STADISCONNECTED && n) n--;
    printer_state_set_ap_stations(n);
}
```

In `app_main`'s setup branch, directly before `        ESP_ERROR_CHECK(net_provision_start(ap));`, add:
```c
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event, NULL, NULL));
```

- [ ] **Step 5: Build**

```bash
make -C tools/rip test
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -E "error|warning: |complete"
```
Expected: tests pass; "Project build complete".

- [ ] **Step 6: Commit**

```bash
git add firmware/main/main.c
git commit -m "main: rejoin Wi-Fi forever after a drop; count phones on the setup AP

Once joined, a disconnect posts WIFI_DOWN and retries on a 1-30 s
backoff instead of giving up after 8 tries and leaving the panel on
Ready. A drop never falls back to the setup AP. In setup mode, station
join/leave events drive the solid-blue 'phone joined' cue.

Refs: W-1037"
```

---

### Task 5: Activity wakes the panel; offline reaches IPP

**Files:**
- Modify: `firmware/components/ipp_server/include/ipp_server.h`
- Modify: `firmware/components/ipp_server/ipp_server.c`
- Modify: `firmware/main/main.c`

**Interfaces:**
- Consumes: `printer_state_post(PS_EV_ACTIVITY)`, `ps_view_t` from Task 2.
- Produces: `ipp_server_cfg_t.on_job_op` (`void (*)(void)`).

- [ ] **Step 1: Add the job-operation hook to `ipp_server`**

In `firmware/components/ipp_server/include/ipp_server.h`, inside `ipp_server_cfg_t`, after `int dev_mode; …`, add:
```c
    // A job operation arrived (Validate-Job, Print-Job): a person is printing,
    // so the panel should wake. Deliberately not called for
    // Get-Printer-Attributes, which Home Assistant polls every 30 s. May be NULL.
    void          (*on_job_op)(void);
```

In `firmware/components/ipp_server/ipp_server.c`, in the dispatch `switch (r->op)`, replace
```c
    case IPP_OP_VALIDATE_JOB:
        if (!format_supported(r->document_format))
```
with
```c
    case IPP_OP_VALIDATE_JOB:
        if (s_cfg.on_job_op) s_cfg.on_job_op();
        if (!format_supported(r->document_format))
```
and replace
```c
    case IPP_OP_PRINT_JOB:
        return op_print_job(w, fd, r, body, pre, pre_len, keep_alive);
```
with
```c
    case IPP_OP_PRINT_JOB:
        if (s_cfg.on_job_op) s_cfg.on_job_op();
        return op_print_job(w, fd, r, body, pre, pre_len, keep_alive);
```

- [ ] **Step 2: Post activity from the person-driven paths**

In `firmware/main/main.c`, directly above `#define TESTPAGE_W 1152` (so it precedes both `test_page` and `dip_on_change`, which call it), add:
```c
// A person did something; wake the panel. From the IPP, DIP and test-button paths.
static void on_activity(void)
{
    printer_state_post(PS_EV_ACTIVITY);
}

```
In `dip_on_change`, make `on_activity();` the first statement of the body.
In `test_page` (the function that takes `s_job_lock` non-blocking), make `on_activity();` the first statement of the body.
In the `ipp_server_cfg_t cfg = { … }` initializer in `app_main`, add `.on_job_op  = on_activity,` after `.dev_mode   = AW_DEV_MODE,`.

- [ ] **Step 3: Map the offline state to IPP**

Replace the `on_ps_change` body's `ipp_server_set_stopped(...)` line with:
```c
    // Mid-job stall: media-empty, the real "out of paper" alert on the phone.
    // Idle and not ready: printer off, unplugged or deselected. offline-report
    // is CUPS-only (not in the IANA registry), so it reports the registered
    // "other".
    ipp_server_set_stopped(v->state == PS_ATTENTION ? "media-empty"
                         : v->state == PS_OFFLINE   ? "other" : NULL);
```

- [ ] **Step 4: Build**

```bash
make -C tools/rip test
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -E "error|warning: |complete"
```
Expected: tests pass; "Project build complete".

- [ ] **Step 5: Commit**

```bash
git add firmware/components/ipp_server firmware/main/main.c
git commit -m "Wake the panel on job requests, DIP and test button; report offline over IPP

Validate-Job and Print-Job post ACTIVITY; Get-Printer-Attributes does
not, because Home Assistant polls it every 30 s and would keep the panel
awake forever. Printer offline reports printer-state stopped with the
registered reason 'other' (offline-report is CUPS-only).

Refs: W-1037"
```

---

### Task 6: LED renderer v2

**Files:**
- Rewrite: `firmware/components/led_grid/include/led_grid_core.h`
- Rewrite: `firmware/components/led_grid/led_grid_core.c`
- Modify: `firmware/components/led_grid/led_grid.c`
- Modify: `firmware/main/main.c`
- Rewrite: `tools/rip/test/test_led_grid.c`

**Interfaces:**
- Consumes: `ps_view_t`, `printer_state_view()` from Task 2.
- Produces: `lg_render(const ps_view_t *v, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3])`, `int lg_pilot_index(void)`, `LG_MASK_CAP`, `LG_BOOT_MS`.

- [ ] **Step 1: Write the failing renderer test**

Replace `tools/rip/test/test_led_grid.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the led_grid frame renderer: caps, which dots each state may
// light, the boot spin, ready's swell, setup's two cues, attention, failure,
// printing's sweep, and the pilot light.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "led_grid_core.h"
#include "printer_state_core.h"

static int max3(const uint8_t px[3])
{
    int m = px[0];
    if (px[1] > m) m = px[1];
    if (px[2] > m) m = px[2];
    return m;
}

static ps_view_t view(ps_state_t st, uint32_t since)
{
    ps_view_t v;
    memset(&v, 0, sizeof v);
    v.state = st;
    v.since_ms = since;
    return v;
}

int main(void)
{
    uint8_t f[LG_NUM_LEDS][3], g[LG_NUM_LEDS][3];
    const ps_state_t all[] = { PS_BOOTING, PS_SETUP, PS_JOINING, PS_ATTENTION,
                               PS_FAILED, PS_PRINTING, PS_OFFLINE, PS_READY };
    const int n_all = (int)(sizeof all / sizeof all[0]);
    const int p = lg_pilot_index();

    // Caps: FR4 dots never exceed LG_BRIGHTNESS_CAP, mask dots LG_MASK_CAP.
    for (int s = 0; s < n_all; s++)
        for (uint32_t t = 0; t < 8000; t += 37) {
            ps_view_t v = view(all[s], t);
            lg_render(&v, t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                for (int ch = 0; ch < 3; ch++)
                    assert(f[i][ch] <= (LG_MAP[i].mat == 'P' ? LG_BRIGHTNESS_CAP
                                                             : LG_MASK_CAP));
        }

    // FR4-only states leave every mask dot dark and light some FR4 dot.
    const ps_state_t fr4_only[] = { PS_SETUP, PS_JOINING, PS_OFFLINE, PS_READY };
    for (int s = 0; s < 4; s++)
        for (uint32_t t = 0; t < 7000; t += 113) {
            ps_view_t v = view(fr4_only[s], t);
            lg_render(&v, t, f);
            int any = 0;
            for (int i = 0; i < LG_NUM_LEDS; i++) {
                if (LG_MAP[i].mat != 'P') assert(max3(f[i]) == 0);
                else if (max3(f[i])) any = 1;
            }
            assert(any);
        }

    // Boot spin: lit mid-way, mask dots included; dark by LG_BOOT_MS.
    {
        ps_view_t v = view(PS_BOOTING, 1000);
        lg_render(&v, 1000, f);
        int fr4 = 0, mask = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (!max3(f[i])) continue;
            if (LG_MAP[i].mat == 'P') fr4 = 1; else mask = 1;
        }
        assert(fr4 && mask);
        v = view(PS_BOOTING, LG_BOOT_MS);
        lg_render(&v, LG_BOOT_MS, f);
        for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) == 0);
    }

    // Ready swells on entry, then settles dimmer but lit.
    {
        ps_view_t a = view(PS_READY, 500), b = view(PS_READY, 5000);
        lg_render(&a, 0, f);
        int pa = max3(f[p]);
        lg_render(&b, 0, f);
        int pb = max3(f[p]);
        assert(pa > pb && pb > 0);
    }

    // Setup with a phone joined is steady; without one it pulses.
    {
        ps_view_t v = view(PS_SETUP, 0);
        v.phone_joined = 1;
        lg_render(&v, 0, f);
        lg_render(&v, 1234, g);
        assert(memcmp(f, g, sizeof f) == 0);
        v.phone_joined = 0;
        lg_render(&v, 0, f);
        lg_render(&v, 1250, g);
        assert(memcmp(f, g, sizeof f) != 0);
    }

    // Attention: amber (red over green, no blue), mask dots too; dark between blinks.
    {
        ps_view_t v = view(PS_ATTENTION, 0);
        lg_render(&v, 50, f);
        int mask = 0;
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (!max3(f[i])) continue;
            assert(f[i][0] > f[i][1] && f[i][2] == 0);
            if (LG_MAP[i].mat != 'P') mask = 1;
        }
        assert(mask);
        lg_render(&v, 1000, f);
        for (int i = 0; i < LG_NUM_LEDS; i++) assert(max3(f[i]) == 0);
    }

    // Failed: a flash on at 100 ms and off at 300 ms, then a red pulse that never goes dark.
    {
        ps_view_t v = view(PS_FAILED, 100);
        lg_render(&v, 0, f);
        assert(f[p][0] > 0 && f[p][0] > f[p][1]);
        v.since_ms = 300;
        lg_render(&v, 0, f);
        assert(max3(f[p]) == 0);
        for (uint32_t s = 1200; s < 6000; s += 100) {
            v.since_ms = s;
            lg_render(&v, 0, f);
            assert(f[p][0] > 0);
        }
    }

    // Printing: the bright row moves down with time.
    {
        int r0 = -1, r4 = -1;
        for (int i = 0; i < LG_NUM_LEDS; i++) {
            if (LG_MAP[i].mat != 'P') continue;
            if (LG_MAP[i].r == 0 && r0 < 0) r0 = i;
            if (LG_MAP[i].r == 4 && r4 < 0) r4 = i;
        }
        assert(r0 >= 0 && r4 >= 0);
        ps_view_t v = view(PS_PRINTING, 0);
        lg_render(&v, 0, f);
        assert(max3(f[r0]) > max3(f[r4]));
        lg_render(&v, 750, f);
        assert(max3(f[r4]) > max3(f[r0]));
    }

    // Asleep: after the 10 s fade, only the pilot -- the FR4 dot right of the
    // test button -- is lit, green when ready, warm white when offline.
    {
        assert(LG_MAP[p].r == 3 && LG_MAP[p].c == 4 && LG_MAP[p].mat == 'P');
        ps_view_t v = view(PS_READY, 400000);
        v.asleep = 1;
        v.asleep_ms = 20000;
        for (uint32_t t = 0; t < 6000; t += 250) {
            lg_render(&v, t, f);
            for (int i = 0; i < LG_NUM_LEDS; i++)
                assert(i == p ? max3(f[i]) > 0 : max3(f[i]) == 0);
            assert(f[p][1] >= f[p][0] && f[p][1] >= f[p][2]);
        }
        v.state = PS_OFFLINE;
        lg_render(&v, 3000, f);
        assert(f[p][0] >= f[p][1] && f[p][1] >= f[p][2]);
    }

    printf("test_led_grid: ok\n");
    return 0;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `make -C tools/rip build/test_led_grid`
Expected: compile errors, such as `'LG_MASK_CAP' undeclared` and `lg_pilot_index` implicitly declared.

- [ ] **Step 3: Write the header**

Replace `firmware/components/led_grid/include/led_grid_core.h` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Pure frame renderer for the 37-dot art panel: (view, t) -> one RGB frame.
// No FreeRTOS, no I/O -- led_grid.c drives the RMT strip from its output.
#pragma once
#include <stdint.h>
#include "led_map.h"
#include "printer_state_core.h"

#define LG_BRIGHTNESS_CAP 48    // of 255, FR4 dots -- no hardware current limit on rev A
#define LG_MASK_CAP       192   // mask dots in the all-dot states: 4x, they pass ~10% / ~5%
#define LG_BOOT_MS        3000  // the boot spin; main sets printer_state's boot_ms to this

void lg_render(const ps_view_t *v, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3]);
int  lg_pilot_index(void);      // the FR4 dot right of the test button (row 3, col 4)
```

- [ ] **Step 4: Write the renderer**

Replace `firmware/components/led_grid/led_grid_core.c` with:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
// Frame renderer for the 37-dot art panel: a pure function of (view, t), so
// the host tests can walk every phase. Material physics measured on board 1
// (docs/superpowers/specs/2026-10-14-status-language-design.md): FR4 dots are
// the crisp pixels; mask dots pass ~10% (B) / ~5% (L), red well, blue as teal.
// So information lives on FR4 dots, and mask dots join in only for the boot
// spin and the printing / attention / failed states, at LG_MASK_CAP. Levels
// are perceptual and go through gamma 2.2 before the cap; the pilot light is
// the exception, driven linearly because gamma would round it to zero.
#include "led_grid_core.h"
#include <math.h>
#include <string.h>

typedef struct { float r, g, b; } lg_c3;

static const lg_c3 BLUE  = {0.10f, 0.35f, 1.00f};
static const lg_c3 GREEN = {0.15f, 1.00f, 0.25f};
static const lg_c3 WARM  = {1.00f, 0.75f, 0.45f};
static const lg_c3 AMBER = {1.00f, 0.55f, 0.00f};
static const lg_c3 RED   = {1.00f, 0.05f, 0.02f};
static const lg_c3 WHITE = {1.00f, 1.00f, 1.00f};
static const lg_c3 DARK  = {0.00f, 0.00f, 0.00f};

static float clampf(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float ss(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
static lg_c3 sc(lg_c3 a, float k) { return (lg_c3){a.r * k, a.g * k, a.b * k}; }

// lo -> hi -> lo over period_ms, smooth at both ends; lo at phase 0.
static float pulse(uint32_t t, uint32_t period_ms, float lo, float hi)
{
    float ph = (float)(t % period_ms) / (float)period_ms;
    return lo + (hi - lo) * (0.5f - 0.5f * cosf(6.2831853f * ph));
}

static lg_c3 hsv(float h, float s, float v)
{
    h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    int i = (int)h;
    float f = h - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0:  return (lg_c3){v, t, p};
    case 1:  return (lg_c3){q, v, p};
    case 2:  return (lg_c3){p, v, t};
    case 3:  return (lg_c3){p, q, v};
    case 4:  return (lg_c3){t, p, v};
    default: return (lg_c3){v, p, q};
    }
}

// Gamma 2.2, then the cap. At a cap of 48 the bottom of a slow breathe would
// round to zero and the dot would blink off, so any lit level gets one count.
static uint8_t drive(float level, float cap)
{
    level = clampf(level);
    if (level <= 0) return 0;
    float o = powf(level, 2.2f) * cap + 0.5f;
    if (o < 1) o = 1;
    return (uint8_t)(o > 255 ? 255 : o);
}

static int all_dots(ps_state_t st)
{
    return st == PS_BOOTING || st == PS_PRINTING || st == PS_ATTENTION || st == PS_FAILED;
}

int lg_pilot_index(void)
{
    for (int i = 0; i < LG_NUM_LEDS; i++)
        if (LG_MAP[i].r == 3 && LG_MAP[i].c == 4) return i;
    return 0;
}

// The boot spin: a hue wheel around the test button (row 3, col 3) that fades
// in by distance, spins, then collapses inward and is dark by LG_BOOT_MS.
static lg_c3 spin(int i, float t)
{
    float dr = (float)LG_MAP[i].r - 3, dc = (float)LG_MAP[i].c - 3;
    float dist = hypotf(dr, dc), ang = atan2f(dr, dc) * 57.29578f;
    float v = ss(0, 0.6f, t - dist * 0.08f) * (1 - ss(2.3f, 3.0f, t + dist * 0.12f));
    return hsv(ang + t * 300 - t * t * 40, 0.9f, v);
}

static lg_c3 state_dot(const ps_view_t *v, int i, uint32_t t)
{
    uint32_t s = v->since_ms;
    switch (v->state) {
    case PS_BOOTING:
        return spin(i, (float)s / 1000.0f);
    case PS_SETUP:
        return v->phone_joined ? sc(BLUE, 0.6f) : sc(BLUE, pulse(t, 2500, 0.1f, 0.8f));
    case PS_JOINING:
        return sc(BLUE, pulse(t, 800, 0.1f, 0.8f));
    case PS_READY:
        if (s < 500)  return sc(GREEN, 0.35f + 0.65f * (float)s / 500.0f);
        if (s < 1500) return sc(GREEN, 1.0f - 0.65f * (float)(s - 500) / 1000.0f);
        return sc(GREEN, 0.35f);
    case PS_OFFLINE:
        return sc(WARM, pulse(t, 6000, 0.12f, 0.30f));
    case PS_PRINTING: {
        int row = (int)((t % 1500) * LG_ROWS / 1500);
        int dr = LG_MAP[i].r - row;
        if (dr < 0) dr = -dr;
        return sc(WHITE, dr == 0 ? 1.0f : dr == 1 ? 0.25f : 0.06f);
    }
    case PS_ATTENTION: {
        uint32_t ph = t % 2000;
        return (ph < 150 || (ph >= 300 && ph < 450)) ? AMBER : DARK;
    }
    case PS_FAILED:
        if (s < 1200) return (s % 400) < 200 ? RED : DARK;
        return sc(RED, pulse(s - 1200, 2000, 0.2f, 0.6f));
    }
    return DARK;
}

void lg_render(const ps_view_t *v, uint32_t t_ms, uint8_t out[LG_NUM_LEDS][3])
{
    memset(out, 0, LG_NUM_LEDS * 3);
    int all = all_dots(v->state);
    float sleep_k = v->asleep ? clampf((float)v->asleep_ms / 10000.0f) : 0;   // 10 s fade

    for (int i = 0; i < LG_NUM_LEDS; i++) {
        int fr4 = LG_MAP[i].mat == 'P';
        if (!fr4 && !all) continue;            // FR4-only state: mask dot stays dark
        lg_c3 c = sc(state_dot(v, i, t_ms), 1 - sleep_k);
        float cap = fr4 ? LG_BRIGHTNESS_CAP : LG_MASK_CAP;
        out[i][0] = drive(c.r, cap);
        out[i][1] = drive(c.g, cap);
        out[i][2] = drive(c.b, cap);
    }

    if (v->asleep) {
        int p = lg_pilot_index();
        lg_c3 col = v->state == PS_OFFLINE ? WARM : GREEN;
        float lvl = pulse(t_ms, 6000, 0.03f, 0.12f) * sleep_k * LG_BRIGHTNESS_CAP;
        uint8_t px[3] = {
            (uint8_t)(col.r * lvl + 0.5f),
            (uint8_t)(col.g * lvl + 0.5f),
            (uint8_t)(col.b * lvl + 0.5f),
        };
        for (int ch = 0; ch < 3; ch++)
            if (px[ch] > out[p][ch]) out[p][ch] = px[ch];
    }
}
```

- [ ] **Step 5: Run the renderer test to verify it passes**

Run: `make -C tools/rip build/test_led_grid && ./tools/rip/build/test_led_grid`
Expected: `test_led_grid: ok`

- [ ] **Step 6: Drive the panel from the view, and set the boot length**

In `firmware/components/led_grid/led_grid.c`, in `lg_task`, replace
```c
        lg_render(printer_state_view().state,
                  (uint32_t)(esp_timer_get_time() / 1000), frame);
```
with
```c
        ps_view_t v = printer_state_view();
        lg_render(&v, (uint32_t)(esp_timer_get_time() / 1000), frame);
```

In `firmware/main/main.c`:
- After `#include "led_grid.h"`, add `#include "led_grid_core.h"`.
- In `app_main`, after `tm.stall_attention_ms = CONFIG_AIRWRITER_STALL_TIMEOUT * 1000u;`, add `tm.boot_ms = LG_BOOT_MS;`.

- [ ] **Step 7: Verify**

```bash
make -C tools/rip test && make -C tools/rip regress
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -E "error|warning: |complete"
```
Expected: all tests pass, both regressions are unchanged, "Project build complete".

- [ ] **Step 8: Commit**

```bash
git add firmware/components/led_grid tools/rip/test/test_led_grid.c firmware/main/main.c
git commit -m "led_grid: status language v2 on the measured panel physics

Information on the 14 FR4 dots; mask dots join only in the boot spin and
the printing / attention / failed states, at 4x. IoT blues for setup and
joining, green when ready (with a swell on entry), warm white when the
printer is offline, amber attention, red failure, and a one-dot pilot
light after five idle minutes. Gamma 2.2 on everything but the pilot.

Refs: W-1037"
```

---

### Task 7: Web status speaks the new states

**Files:**
- Modify: `firmware/main/main.c` (`status_json`)
- Modify: `tools/webui/app.js`
- Regenerate: `firmware/components/webui/webui_page.h`

**Interfaces:**
- Consumes: `printer_state_view()`, `ps_name()` from Task 2.
- Produces: status JSON keys `state` (human text), `ps` (machine name), new `asleep` (bool). `stopped` = attention or offline; `fault` = attention or failed; `printer` = live HSKo.

- [ ] **Step 1: Rewrite the state part of `status_json`**

In `firmware/main/main.c`, directly above `static void status_json(`, add:
```c
static const char *state_text(ps_state_t st)
{
    switch (st) {
    case PS_BOOTING:   return "Starting";
    case PS_SETUP:     return "Waiting for setup";
    case PS_JOINING:   return "Joining Wi-Fi";
    case PS_ATTENTION: return "Needs attention";
    case PS_FAILED:    return "Last job failed";
    case PS_PRINTING:  return "Printing";
    case PS_OFFLINE:   return "Printer offline";
    case PS_READY:     return "Ready";
    }
    return "?";
}

```
In `status_json`, replace
```c
    ps_view_t v = printer_state_view();
    int blocked = v.state == PS_ATTENTION || v.state == PS_OFFLINE;
```
with
```c
    ps_view_t v = printer_state_view();
    int attention = v.state == PS_ATTENTION, offline = v.state == PS_OFFLINE;
    int failed    = v.state == PS_FAILED;
```
In the format string, replace `"{\"state\":\"%s\",\"ps\":\"%s\",\"stopped\":%s,` with `"{\"state\":\"%s\",\"ps\":\"%s\",\"asleep\":%s,\"stopped\":%s,`.
Replace the first four arguments after the format string:
```c
        s_ap_mode ? "Waiting for setup" : blocked ? "Out of paper"
                : s_printing ? "Printing" : "Ready",
        ps_name(v.state),
        blocked ? "true" : "false",
```
with
```c
        state_text(v.state),
        ps_name(v.state),
        v.asleep ? "true" : "false",
        attention || offline ? "true" : "false",
```
and replace the `fault` argument (the second `blocked ? "true" : "false",`, after `job,`) with:
```c
        attention || failed ? "true" : "false",
```

- [ ] **Step 2: Update the page's lamps**

In `tools/webui/app.js`, inside `function draw(s)`, replace from the comment `/* Nothing on the serial port is a fault, not an idle state: …` through the line `                           :'printer connected and online';` with:
```js
  /* A printer that is off or unplugged is not a fault -- the board is fine and
     says so on its panel in warm white. Faults are the two states that need a
     person: a job stalled mid-page, or one that failed. */
  const ps=s.ps||'', offline=ps==='offline';
  const fault=ps==='attention'||ps==='failed';
  lamp($('ledfault'),fault,'r',
    ps==='attention'?'needs attention: paper, jam or SELECT':
    ps==='failed'?'the last job failed':'no fault');
  lamp($('ledlink'),!setup,'g',setup?'not on a network':'joined “'+(s.ssid||'')+'”');
  const prn=$('ledprn');
  prn.classList.toggle('g',!!s.printer);
  prn.classList.remove('r');
  prn.dataset.tip=s.printer?'printer connected and ready'
                 :offline?'printer offline: off, unplugged or deselected'
                 :'printer busy';
```

Regenerate the embedded page:
```bash
python3 tools/webui/mkheader.py
git diff --stat firmware/components/webui/webui_page.h
```
Expected: `webui_page.h` changes.

- [ ] **Step 3: Build**

```bash
make -C tools/rip test
. ~/esp/esp-idf/export.sh >/dev/null 2>&1 && idf.py -C firmware build 2>&1 | grep -E "error|warning: |complete"
```
Expected: tests pass; "Project build complete".

- [ ] **Step 4: Commit**

```bash
git add firmware/main/main.c tools/webui/app.js firmware/components/webui/webui_page.h
git commit -m "webui: status names follow the new states; offline is not a fault

status JSON reports the human state ('Needs attention', 'Printer
offline', ...), the machine name and asleep. The page lights the fault
lamp only for attention or a failed job, and tells offline apart from
busy on the printer lamp.

Refs: W-1037"
```

---

### Task 8: Verify on board 1, then record it

Needs Wells at the bench for the physical steps. The board is on the home Wi-Fi at `imagewriter-2434.local` (10.0.1.115), with the ImageWriter II on J2.

**Files:**
- Modify: `docs/HANDOFF.md`

- [ ] **Step 1: Flash over the air and confirm the version**

```bash
curl -s -m 150 -w "\nhttp=%{http_code}\n" -H "Content-Type: application/octet-stream" \
  --data-binary @firmware/build/airwriter.bin http://imagewriter-2434.local/update
for i in $(seq 1 40); do v=$(curl -s -m 2 http://imagewriter-2434.local/api/status | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['version'],d['ps'])" 2>/dev/null); case "$v" in *"$(git rev-parse --short HEAD)"*) break;; esac; sleep 2; done; echo "$v"
```
Expected: `http=200`, then the HEAD hash and a state name. If the upload drops mid-transfer (seen once on 2026-10-14), run it again.

- [ ] **Step 2: Walk the spec's board checks with Wells, recording each result**

Watch transitions with:
```bash
prev=""; for i in $(seq 1 1200); do cur=$(curl -s -m 2 http://imagewriter-2434.local/api/status | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['ps'],'asleep='+str(d['asleep']),'hsk='+str(d['printer']))" 2>/dev/null || echo no-answer); [ "$cur" != "$prev" ] && echo "$(date +%T) $cur" && prev="$cur"; sleep 0.5; done
```
1. Power-cycle the board: Spin, then blue quick pulse, then green swell, then steady green.
2. Pull the printer cable: `offline` within ~3 s, warm-white breathe. Reconnect: `ready`.
3. Print from the iPhone, and press SELECT off mid-page: `attention` after 30 s, amber double-blink, the phone shows out of paper. Press SELECT on: `printing`, and the page completes intact. Note what iOS showed during the pause and whether its upload survived.
4. With the printer off, open the iPhone print sheet: note what it shows for printer-state `stopped` + `other`.
5. Turn Wi-Fi off at the router for 1 minute: `joining` (blue quick pulse), no `AirWriter-…` network appears; back on: `ready`.
6. Leave everything alone for 5 minutes with Home Assistant running: `asleep=True`, a single green pilot dot. A Print from the phone or the test button wakes it.
7. Factory setup path (optional): with no saved Wi-Fi, a blue slow pulse; join `AirWriter-2434` from a phone: solid blue.

If a level or colour looks wrong on the panel, adjust only the constants in `led_grid_core.c` (`BLUE`, `GREEN`, `WARM`, `AMBER`, `RED`, the `pulse()` levels), re-run `make -C tools/rip test`, rebuild, re-flash, and commit as `led_grid: tune <state> on board 1`.

- [ ] **Step 3: Record the results in HANDOFF**

In `docs/HANDOFF.md`, at the top of "Where the project stands", add a dated entry:
```markdown
**2026-10-1x: status language v2 is on the board (W-1037).** Spec
`docs/superpowers/specs/2026-10-14-status-language-design.md`, plan
`docs/superpowers/plans/2026-10-14-status-language.md`. Eight display states from a pure
reducer (`printer_state_core`); a stalled job pauses up to `AIRWRITER_STALL_ABORT` (600 s)
instead of aborting; Wi-Fi rejoins forever; the panel sleeps to a pilot dot after 5 min.
Board checks: <one line per Step 2 item with the observed result, including what iOS showed
during a pause and for `stopped`+`other`>.
```
Fill in the real date and the observed results. Then commit:
```bash
git add docs/HANDOFF.md
git commit -m "HANDOFF: status language v2 verified on board 1

Refs: W-1037"
```

- [ ] **Step 4: Report**

Post a Linear comment on W-1037 summarising the board results, mentioning @wells, and tell Wells the branch is ready. Do not merge: Wells merges on "merge it".
