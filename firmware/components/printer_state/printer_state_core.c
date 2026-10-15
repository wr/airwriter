// SPDX-License-Identifier: GPL-3.0-or-later
// Device-state reducer. Pure C on purpose -- the ESP glue and the host tests
// share this file. Priority does the work; there is no transition table.
#include "printer_state_core.h"
#include <string.h>

void ps_init(ps_t *ps, const ps_timing_t *tm, uint64_t now)
{
    memset(ps, 0, sizeof *ps);
    ps->tm = *tm;
    ps->boot_at = ps->line_since = ps->activity_at = ps->state_since = now;
    ps->last = PS_BOOTING;
}

void ps_post(ps_t *ps, ps_event_t ev, uint64_t now)
{
    switch (ev) {
    case PS_EV_WIFI_SETUP_AP:  ps->setup = 1;   break;
    case PS_EV_WIFI_JOINING:
    case PS_EV_WIFI_DOWN:      ps->wifi_up = 0; break;
    case PS_EV_WIFI_UP:        ps->wifi_up = 1; break;
    case PS_EV_LINE_READY:
        if (!ps->line_ready) { ps->line_ready = 1; ps->line_since = now; ps->kicked = 0; }
        ps->job_started_low = 0;
        break;
    case PS_EV_LINE_NOT_READY:
        if (ps->line_ready) { ps->line_ready = 0; ps->line_since = now; }
        break;
    case PS_EV_JOB_START:
        ps->job = 1; ps->failed = 0; ps->activity_at = now;
        ps->job_started_low = !ps->line_ready;
        break;
    case PS_EV_JOB_DONE:   ps->job = 0; break;
    case PS_EV_JOB_FAILED: ps->job = 0; ps->failed = 1; ps->failed_at = now; break;
    case PS_EV_ACTIVITY:   ps->activity_at = now; break;
    }
}

void ps_set_ap_stations(ps_t *ps, unsigned n)
{
    ps->ap_stations = (uint8_t)(n > 255 ? 255 : n);
}

static ps_state_t derive(const ps_t *ps, uint64_t now)
{
    if (now - ps->boot_at < ps->tm.boot_ms) return PS_BOOTING;
    if (ps->setup)    return PS_SETUP;
    if (!ps->wifi_up) return PS_JOINING;
    uint64_t low_for = ps->line_ready ? 0 : now - ps->line_since;
    // Mid-page the ImageWriter drops the line constantly, so attention waits
    // 30 s. A line already down when the job began means no printer (after an
    // idle spell its buffer is empty): ask after the offline debounce instead.
    uint32_t attention_after = ps->job_started_low ? ps->tm.offline_debounce_ms
                                                   : ps->tm.stall_attention_ms;
    if (ps->job && !ps->line_ready && low_for >= attention_after)
        return PS_ATTENTION;
    if (ps->failed && now - ps->failed_at < ps->tm.failed_hold_ms) return PS_FAILED;
    if (ps->job) return PS_PRINTING;
    // Offline is counted from the end of the boot animation, not power-on: the
    // ImageWriter reports busy for ~3.5 s after the AirWriter powers up and
    // then clears by itself (W-1039), which must not flash embers after the
    // spin or swallow the green Wi-Fi moment.
    uint64_t settled = ps->boot_at + ps->tm.boot_ms;
    uint64_t from = ps->line_since > settled ? ps->line_since : settled;
    if (!ps->line_ready && now >= from && now - from >= ps->tm.offline_debounce_ms)
        return PS_OFFLINE;
    return PS_READY;
}

static uint32_t sat32(uint64_t x) { return x > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)x; }

ps_view_t ps_update(ps_t *ps, uint64_t now)
{
    ps_state_t st = derive(ps, now);
    if (st != ps->last) {
        if (st == PS_READY)
            ps->greeted = ps->last == PS_JOINING || ps->last == PS_BOOTING;
        ps->last = st;
        ps->state_since = now;
        ps->activity_at = now;
    }
    ps_view_t v = { .state = st, .since_ms = sat32(now - ps->state_since) };
    v.phone_joined = st == PS_SETUP && ps->ap_stations > 0;
    v.greeted      = st == PS_READY && ps->greeted;
    if ((st == PS_READY || st == PS_OFFLINE) &&
        now - ps->activity_at >= ps->tm.sleep_after_ms) {
        v.asleep    = 1;
        v.asleep_ms = sat32(now - ps->activity_at - ps->tm.sleep_after_ms);
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

int ps_want_kick(const ps_t *ps, uint64_t now)
{
    if (ps->job || ps->line_ready || ps->setup) return 0;
    if (now - ps->line_since < ps->tm.kick_after_ms) return 0;
    return !ps->kicked || now - ps->kicked_at >= ps->tm.kick_every_ms;
}

void ps_kicked(ps_t *ps, uint64_t now)
{
    ps->kicked = 1;
    ps->kicked_at = now;
}
