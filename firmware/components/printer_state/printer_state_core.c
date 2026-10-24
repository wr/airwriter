// SPDX-License-Identifier: GPL-3.0-or-later
// Device-state reducer. Pure C on purpose -- the ESP glue and the host tests
// share this file. Priority does the work; there is no transition table.
#include "printer_state_core.h"
#include <string.h>

void ps_init(ps_t *ps, const ps_timing_t *tm, uint64_t now)
{
    memset(ps, 0, sizeof *ps);
    ps->tm = *tm;
    ps->boot_at = ps->line_since = ps->activity_at = ps->state_since = ps->port_since = now;
    ps->last = ps->prev = PS_BOOTING;
}

static uint64_t max64(uint64_t a, uint64_t b) { return a > b ? a : b; }
static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

// W-1049's quiet power-on: when the Spin starts going out, or 0 while it is
// still waiting at `now`. The board has settled once Wi-Fi is up and the
// printer line has held still (ready a moment, or down long enough to be no
// printer), or at once on setup mode or a job. The Spin shows at least
// boot_ms and at most boot_cap_ms, going out for the last boot_out_ms.
static uint64_t out_start(const ps_t *ps, uint64_t now)
{
    const ps_timing_t *tm = &ps->tm;
    uint64_t first = ps->boot_at + (tm->boot_ms > tm->boot_out_ms ? tm->boot_ms - tm->boot_out_ms : 0);
    uint64_t last  = ps->boot_at + (tm->boot_cap_ms > tm->boot_out_ms ? tm->boot_cap_ms - tm->boot_out_ms : 0);
    uint64_t at = last;
    if (ps->setup) at = min64(at, ps->setup_at);
    if (ps->job)   at = min64(at, ps->job_at);
    if (ps->wifi_up) {
        uint64_t line = ps->line_since + (ps->line_ready ? tm->settle_ready_ms : tm->settle_offline_ms);
        at = min64(at, max64(ps->wifi_at, line));
    }
    at = max64(at, first);
    return at <= now ? at : 0;
}

// Latches the Spin's going-out once the board has settled. Run before each
// event, so the moment is judged on what held until then, and on each update.
static void latch_out(ps_t *ps, uint64_t now)
{
    if (ps->out) return;
    uint64_t at = out_start(ps, now);
    if (at) { ps->out = 1; ps->out_at = at; }
}

void ps_post(ps_t *ps, ps_event_t ev, uint64_t now)
{
    latch_out(ps, now);
    switch (ev) {
    case PS_EV_WIFI_SETUP_AP:
        ps->setup = 1; ps->setup_at = now;
        ps->join_failed = ps->tried;    // JOINING is posted only when there is a network to try
        break;
    case PS_EV_WIFI_JOINING:   ps->wifi_up = 0; ps->tried = 1; break;
    case PS_EV_WIFI_DOWN:      ps->wifi_up = 0; break;
    case PS_EV_WIFI_UP:
        if (!ps->wifi_up) ps->wifi_at = now;
        ps->wifi_up = 1;
        break;
    case PS_EV_WIFI_FRESH:     ps->fresh = 1; break;
    case PS_EV_LINE_READY:
        if (!ps->line_ready) { ps->line_ready = 1; ps->line_since = now; ps->kicked = 0; }
        ps->job_started_low = 0;
        break;
    case PS_EV_LINE_NOT_READY:
        if (ps->line_ready) { ps->line_ready = 0; ps->line_since = now; }
        break;
    case PS_EV_JOB_START:
        ps->job = 1; ps->failed = 0; ps->cancelled = 0; ps->activity_at = now;
        ps->job_at = now;
        ps->job_started_low = !ps->line_ready;
        break;
    case PS_EV_JOB_DONE:   ps->job = 0; break;
    case PS_EV_JOB_CANCELLED: ps->job = 0; ps->cancelled = 1; break;
    case PS_EV_JOB_FAILED: ps->job = 0; ps->failed = 1; ps->failed_at = now; break;
    case PS_EV_ACTIVITY:   ps->activity_at = now; break;
    }
}

void ps_set_ap_stations(ps_t *ps, unsigned n)
{
    ps->ap_stations = (uint8_t)(n > 255 ? 255 : n);
}

void ps_cable(ps_t *ps, uint8_t result, uint64_t now)
{
    ps->cable = result;
    ps->cable_at = now;
    ps->activity_at = now;
}

static ps_state_t derive(const ps_t *ps, uint64_t now)
{
    if (!ps->out || now - ps->out_at < ps->tm.boot_out_ms) return PS_BOOTING;
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
    // The ImageWriter reports busy for ~3.5 s after the AirWriter powers up
    // and then clears by itself (W-1039). The Spin waits for the line to
    // settle (out_start), so after it a down line is a real one: the embers
    // come straight up for a printer that never came, with no Ready first.
    if (!ps->line_ready && now - ps->line_since >= ps->tm.offline_debounce_ms)
        return PS_OFFLINE;
    return PS_READY;
}

static uint32_t sat32(uint64_t x) { return x > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)x; }

ps_view_t ps_update(ps_t *ps, uint64_t now)
{
    latch_out(ps, now);
    ps_state_t st = derive(ps, now);
    if (st != ps->last) {
        // The green Wi-Fi moment is for a real join, or the first one after
        // Wi-Fi was set up; a normal power-on is the Spin alone (W-1049).
        if (st == PS_READY)
            ps->greeted = ps->last == PS_JOINING || (ps->last == PS_BOOTING && ps->fresh);
        ps->prev = ps->last;
        ps->last = st;
        ps->state_since = now;
        ps->activity_at = now;
    }
    ps_view_t v = { .state = st, .prev = ps->prev, .cancelled = ps->cancelled,
                    .since_ms = sat32(now - ps->state_since) };
    v.phone_joined = st == PS_SETUP && ps->ap_stations > 0;
    v.join_failed  = st == PS_SETUP && ps->join_failed;
    if (st == PS_BOOTING && ps->out && now >= ps->out_at) {
        v.spin_out    = 1;
        v.spin_out_ms = sat32(now - ps->out_at);
    }
    v.greeted      = st == PS_READY && ps->greeted;
    uint32_t cable_hold = ps->cable == PS_CABLE_NO_CHIP ? PS_CABLE_NO_CHIP_MS : PS_CABLE_MS;
    if (ps->cable && now - ps->cable_at < cable_hold &&
        (st == PS_SETUP || st == PS_JOINING || st == PS_READY || st == PS_OFFLINE)) {
        v.cable    = ps->cable;
        v.cable_ms = sat32(now - ps->cable_at);
    }
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
    if (ps->job || ps->port_ready || ps->setup) return 0;
    if (now - ps->port_since < ps->tm.kick_after_ms) return 0;
    return !ps->kicked || now - ps->kicked_at >= ps->tm.kick_every_ms;
}

void ps_port_line(ps_t *ps, int ready, uint64_t now)
{
    ready = ready ? 1 : 0;
    if (ready == ps->port_ready) return;
    ps->port_ready = (uint8_t)ready;
    ps->port_since = now;
    if (ready) ps->kicked = 0;
}

void ps_kicked(ps_t *ps, uint64_t now)
{
    ps->kicked = 1;
    ps->kicked_at = now;
}
