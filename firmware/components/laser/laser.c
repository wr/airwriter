// SPDX-License-Identifier: GPL-3.0-or-later
// The AppleTalk printers (W-1082, W-1088); see laser.h.
//
// Locks, in the only order they are taken: s_client_mx (who uses the PAP
// client, and which link it rides), then a link's own lock (localtalk_locked,
// ethertalk_locked), then s_mu (everything else here). The client's callbacks
// run on a link's task under that link's lock, so they take s_mu alone and
// never call out: what they learn the laser task acts on. The printer list's
// callbacks (laser_cfg_t) are called with none of these held; it may call in
// here holding its own lock, never the other way round.
#include "laser.h"
#include "lw_core.h"
#include "lw_query.h"
#include "lw_caps.h"
#include "ps_page.h"
#include "ps_testpage.h"
#include "localtalk.h"
#include "ethertalk.h"
#include "pap_client.h"
#include "pap_relay.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "laser";

#define STALL_MS   (CONFIG_AIRWRITER_STALL_ABORT * 1000u)
#define SPOOL      (16 * 1024)       // between the rip and the client: 4 SendData answers
#define QUERY_BUSY_MS 60000          // a query waits out a busy printer this long
#define INFO_VER   1
// The first look waits (laser_cfg_t's first_look_ms) for Wi-Fi and Infinite
// Mac's TLS handshake to settle: lookups and their answers take Wi-Fi buffers
// from internal RAM, which the handshake's AES needs too (board 1, W-1088: it
// failed once at boot). Each look sends its three types this far apart, and
// all of them again LOOK_AGAIN_MS later: a lookup is one broadcast, and a
// cable can lose it.
#define LOOK_STEP_MS  300
#define LOOK_AGAIN_MS 1500

enum { USE_NONE, USE_QUERY, USE_JOB, USE_RELAY };
#define RELAY_BUF  (16 * 1024)       // each way, in PSRAM

static laser_cfg_t       s_cfg;
static SemaphoreHandle_t s_mu, s_client_mx, s_ended_sem;
static TaskHandle_t      s_task;
static int               s_started;

// A target as the board keeps it, beside lw_core's t[] of the same index.
typedef struct {
    uint8_t     id, kind;
    int8_t      where, slot;
    uint8_t     name[NBP_NAME_MAX + 1];
    char        name_utf8[3 * NBP_NAME_MAX + 1];
    char        dither[12];
    uint8_t     gain, have_info;
    lw_info_t   info;
    lw_caps_t   caps;
    uint8_t     raw_status[256];    // its last status as it sent it (Pascal, MacRoman)
    // What its slot was last told (the laser task's).
    int8_t      pushed_slot, pushed_state, pushed_busy;
    const char *pushed_reason;
    char        pushed_msg[240];
} tgt_t;

// The bulk of this file's state lives in PSRAM (laser_start allocates it):
// Wi-Fi and lwIP take their buffers from internal RAM, and with ~20 KB more
// of it used every TCP reply over one segment stalled on board 1 (W-1082
// bench, 2026-10-22), as with the relay's TLS buffers (docs/traps.md).
typedef struct {
    lwc_t      core;                                // under s_mu
    tgt_t      tg[LWC_TARGETS];                     // under s_mu
    tgt_t      was[LWC_TARGETS];                    // laser_set_targets' scratch
    int        n_tg;
    lw_caps_t  job_caps;                            // the job's, steady while it prints
    pap_relay_t relay;                              // W-1088: a Mac's job for another link's printer
    lw_parse_t parse;                               // the query's answers (link task, then ours)
    uint8_t    query[4096];                         // the query job, probe and padding
} big_t;
static big_t *s_big;
#define s_core  (s_big->core)
#define s_tg    (s_big->tg)
#define s_parse (s_big->parse)

// Under s_mu.
static char      s_ps_error[128];
static char      s_last[48] = "";                   // the last job's end
// Where the last job's time went (W-1082 bench): the rip waiting on a full
// spool is the link; the printer waiting on us is the rip or the upload.
typedef struct {
    uint32_t ms, rip_wait_ms, printer_wait_ms, reads, resends, ram_low;
} job_stats_t;
static job_stats_t s_stats;
static uint64_t    s_job_t0;
static int       s_status_id = -1;                  // whose status was asked last
static int       s_open_id = -1;                    // whom an OpenConn is for
static int       s_job_id = -1, s_job_slot = -1;    // the job's target and slot
static char      s_job_name[3 * NBP_NAME_MAX + 1];
static int       s_look_now;

// The relay (W-1088): whose printer, the Mac's link and node, the job lock.
static SemaphoreHandle_t s_relay_mx;
static volatile int s_relay_id = -1, s_relay_mac_link = -1;
static const lt_node_t *volatile s_relay_node;
static int       s_relay_locked;

// The laser task's.
static int       s_pc_link = -1;
static int       s_query_id = -1;                   // whom the query in flight asked (review M12)
static laser_at_t s_query_at;                       // and where (W-1098)
// A job's OpenConn, run on the laser task (its stack is sized for the send
// path; an IPP worker's is not: review I5).
static volatile int s_open_req, s_open_rc;
static lt_route_t s_open_route;
static SemaphoreHandle_t s_open_sem;
static int       s_ready = -2;
static int       s_links[3] = { -1, -1, -1 };

// The client and its connection.
static pap_client_t *s_pc;
static StreamBufferHandle_t s_spool;
static StaticStreamBuffer_t s_spool_cb;
static volatile int s_use = USE_NONE;
static volatile int s_ended = -1;                   // papc_end_t of the connection
static volatile int s_spool_eof, s_job_abort, s_job_failed, s_cancel_req;
static volatile uint32_t s_sent;
static char      s_line[160];                       // a job's output, a line at a time
static size_t    s_line_n;
static ps_t     *s_ps;
static rip_stage_t s_stage;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static const char *link_name(int link)
{
    switch (link) {
    case AT_LINK_CABLE:  return "cable";
    case AT_LINK_TUNNEL: return "tunnel";
    case AT_LINK_ZONE:   return "zone";
    case AT_LINK_RADIO:  return "radio";
    default:             return "";
    }
}

static void lock(void)   { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

// The t[] index of target id, or -1. Under s_mu.
static int ti_of(int id) { return id < 0 ? -1 : lwc_target(&s_core, id); }

// ------------------------------------------------------------- the client's links

typedef struct { int op; lt_route_t r; uint32_t busy; int rc; } call_t;
enum { OP_OPEN, OP_OPEN_MULTI, OP_STATUS, OP_CANCEL, OP_CLOSE, OP_FINISH, OP_DOWN };

static void do_call(void *arg)
{
    call_t *c = arg;
    uint64_t now = now_ms();
    switch (c->op) {
    case OP_OPEN:   c->rc = papc_open(s_pc, &c->r, c->busy, now); break;
    case OP_OPEN_MULTI: c->rc = papc_open_multi(s_pc, &c->r, c->busy, now); break;
    case OP_STATUS: papc_status(s_pc, &c->r, now); break;
    case OP_CANCEL: papc_cancel(s_pc, now); break;
    case OP_CLOSE:  papc_close(s_pc, now); break;
    case OP_FINISH: papc_finish(s_pc, now); break;
    case OP_DOWN:   papc_link_down(s_pc, now); break;
    }
}

// Runs a client call on the link it rides. Holds s_client_mx.
static int call(int op, const lt_route_t *r, uint32_t busy)
{
    call_t c = { .op = op, .busy = busy, .rc = -1 };
    if (r) c.r = *r;
    if (s_pc_link < 0) return -1;
    int ran = s_pc_link == AT_LINK_CABLE ? localtalk_locked(do_call, &c)
                                         : ethertalk_locked(do_call, &c);
    return ran < 0 ? -1 : op == OP_OPEN || op == OP_OPEN_MULTI ? c.rc : 0;
}

// The client rides `link` from now: only while it has no connection. Holds s_client_mx.
static void ride(int link)
{
    if (link == s_pc_link) return;
    if (s_pc_link == AT_LINK_CABLE) localtalk_attach(NULL);
    else if (s_pc_link >= 0) ethertalk_attach(s_pc_link, NULL);
    if (link == AT_LINK_CABLE) localtalk_attach(s_pc);
    else if (link >= 0) ethertalk_attach(link, s_pc);
    s_pc_link = link;
}

// Empties the spool. Only while no connection reads it (it has one reader).
static void spool_reset(void)
{
    uint8_t junk[64];
    while (xStreamBufferReceive(s_spool, junk, sizeof junk, 0) > 0) {}
    s_spool_eof = 0;
}

// ------------------------------------------------------------- client callbacks
// On a link's task, its lock held.

static size_t pc_read(void *ctx, uint8_t *buf, size_t max, int *eof)
{
    (void)ctx;
    if (s_use == USE_RELAY) return pap_relay_prn_read(&s_big->relay, buf, max, eof);
    if (s_use == USE_NONE) {
        *eof = 1;
        return 0;
    }
    size_t n = xStreamBufferReceive(s_spool, buf, max, 0);
    s_sent += (uint32_t)n;
    *eof = s_spool_eof && xStreamBufferIsEmpty(s_spool);
    return n;
}

static void pc_output(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    if (s_use == USE_RELAY) {
        pap_relay_prn_output(&s_big->relay, d, n);
        return;
    }
    if (s_use == USE_QUERY) {
        lw_parse_feed(&s_parse, d, n);
        return;
    }
    for (size_t k = 0; k < n; k++) {
        if (d[k] != '\r' && d[k] != '\n') {
            if (s_line_n < sizeof s_line - 1) s_line[s_line_n++] = (char)d[k];
            continue;
        }
        if (!s_line_n) continue;
        s_line[s_line_n] = '\0';
        s_line_n = 0;
        ESP_LOGI(TAG, "printer: %s", s_line);
        char msg[sizeof s_ps_error];
        if (lw_ps_error(s_line, msg, sizeof msg)) {
            lock();
            if (!s_ps_error[0]) memcpy(s_ps_error, msg, sizeof s_ps_error);
            unlock();
        }
    }
}

static void pc_status(void *ctx, const uint8_t *pstr)
{
    (void)ctx;
    lock();
    // A busy OpenConn's status is its printer's; a SendStatus answer is for
    // whichever printer was asked last.
    int id = s_pc->phase == PAPC_OPENING ? s_open_id : s_status_id;
    int ti = ti_of(id);
    lwc_status(&s_core, ti, pstr, now_ms());
    if (ti >= 0) memcpy(s_tg[ti].raw_status, pstr, 1 + (size_t)pstr[0]);
    unlock();
    if (s_use == USE_RELAY && id == s_relay_id) pap_relay_prn_status(&s_big->relay, pstr);
}

static void pc_opened(void *ctx)
{
    (void)ctx;
    if (s_use == USE_RELAY) pap_relay_prn_opened(&s_big->relay);
}

// The printer's EOF for one of a relayed connection's jobs (W-1095).
static void pc_job_eof(void *ctx)
{
    (void)ctx;
    if (s_use == USE_RELAY) pap_relay_prn_eof(&s_big->relay);
}

static void pc_ended(void *ctx, papc_end_t how)
{
    (void)ctx;
    if (s_use == USE_RELAY) {
        pap_relay_prn_ended(&s_big->relay, how);
        if (s_task) xTaskNotifyGive(s_task);
        return;
    }
    s_ended = (int)how;
    xSemaphoreGive(s_ended_sem);
    if (s_task) xTaskNotifyGive(s_task);
}

static void pc_log(void *ctx, const char *line)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", line);
}

static void found(int link, const nbp_tuple_t *t, const lt_route_t *r)
{
    lock();
    lwc_found(&s_core, t, (uint8_t)link, r, now_ms());
    unlock();
}

// W-1098: a printer switched on checks its name is free: look a moment after.
// On the link's task.
static void name_check(int link, const nbp_tuple_t *t)
{
    lock();
    uint64_t was = s_core.check_look_at;
    lwc_name_check(&s_core, t, (uint8_t)link, now_ms());
    int armed = s_core.check_look_at != was;
    unlock();
    if (!armed) return;
    char n[3 * NBP_NAME_MAX + 1], ty[3 * NBP_NAME_MAX + 1];
    nbp_utf8(n, sizeof n, t->obj);
    nbp_utf8(ty, sizeof ty, t->type);
    ESP_LOGI(TAG, "\"%s\" (%s) checks its name on the %s: looking in %d s", n, ty, link_name(link),
             LW_CHECK_SETTLE_MS / 1000);
}

// ------------------------------------------------------------- targets

void laser_set_targets(const laser_target_t *in, int n)
{
    if (!s_started) return;
    if (n > LWC_TARGETS) n = LWC_TARGETS;
    lwc_target_in_t ci[LWC_TARGETS];
    lock();
    tgt_t *old = s_big->was;
    int old_n = s_big->n_tg;
    memcpy(old, s_tg, sizeof s_big->was);
    for (int k = 0; k < n; k++) {
        ci[k] = (lwc_target_in_t){ .id = in[k].id, .kind = in[k].kind };
        memcpy(ci[k].name, in[k].name, sizeof ci[k].name);
        tgt_t *g = &s_tg[k];
        memset(g, 0, sizeof *g);
        g->id = in[k].id;
        g->kind = in[k].kind;
        g->where = in[k].where;
        g->slot = in[k].slot;
        memcpy(g->name, in[k].name, sizeof g->name);
        nbp_utf8(g->name_utf8, sizeof g->name_utf8, g->name);
        snprintf(g->dither, sizeof g->dither, "%s", in[k].dither[0] ? in[k].dither : "atkinson");
        g->gain = in[k].gain;
        g->have_info = in[k].have_info;
        g->info = in[k].info;
        if (g->have_info) lw_caps_fill(&g->caps, &g->info);
        g->pushed_slot = -1;                       // its slot hears the whole state again
        for (int j = 0; j < old_n; j++)
            if (old[j].id == g->id && old[j].slot == g->slot && g->slot > 0) {
                g->pushed_slot = old[j].pushed_slot;
                g->pushed_state = old[j].pushed_state;
                g->pushed_busy = old[j].pushed_busy;
                g->pushed_reason = old[j].pushed_reason;
                memcpy(g->pushed_msg, old[j].pushed_msg, sizeof g->pushed_msg);
            }
    }
    s_big->n_tg = n;
    lwc_targets(&s_core, ci, n, now_ms());
    unlock();
    if (s_task) xTaskNotifyGive(s_task);
}

void laser_look_now(void)
{
    if (!s_started) return;
    lock();
    lwc_look_now(&s_core, now_ms());
    s_look_now = 1;
    unlock();
    if (s_task) xTaskNotifyGive(s_task);
}

// ------------------------------------------------------------- the query

static void query_start(int id, int link, const lt_route_t *r)
{
    uint8_t *q = s_big->query;
    size_t n = lw_query_job(q, sizeof s_big->query);
    xSemaphoreTake(s_client_mx, portMAX_DELAY);
    if (s_use != USE_NONE) {
        xSemaphoreGive(s_client_mx);
        return;
    }
    ride(link);
    spool_reset();
    xStreamBufferSend(s_spool, q, n, 0);
    s_spool_eof = 1;
    lw_parse_init(&s_parse);
    s_ended = -1;
    xSemaphoreTake(s_ended_sem, 0);
    s_use = USE_QUERY;
    s_query_id = id;
    s_query_at = (laser_at_t){ .link = link, .net = r->addr.net, .node = r->addr.node, .pages = -1 };
    lock();
    s_status_id = s_open_id = id;
    unlock();
    int rc = call(OP_OPEN, r, QUERY_BUSY_MS);
    xSemaphoreGive(s_client_mx);
    if (rc) {
        s_use = USE_NONE;
        lock();
        lwc_queried(&s_core, ti_of(id), 0, now_ms());
        unlock();
        if (s_cfg.queried) s_cfg.queried(id, 0, NULL, NULL);
        return;
    }
    ESP_LOGI(TAG, "asking printer %d what it is", id);
}

static void query_done(void)
{
    int ok = s_ended == PAPC_END_OK && s_parse.info.complete;
    int id = s_query_id;
    lock();
    int ti = ti_of(id);
    // Taken off the list since it was asked: its answers go nowhere (review M12).
    if (ti < 0) {
        unlock();
        ESP_LOGW(TAG, "query: answered for printer %d, no longer in the list; dropped", id);
        s_use = USE_NONE;
        return;
    }
    if (ok) {
        tgt_t *g = &s_tg[ti];
        g->info = s_parse.info;
        g->have_info = 1;
        lw_caps_fill(&g->caps, &g->info);
    }
    lwc_queried(&s_core, ti, ok, now_ms());
    unlock();
    if (ok) {
        ESP_LOGI(TAG, "query: %s, PostScript %s level %u, %s, %u dpi, %s, %ld pages", s_parse.info.product,
                 s_parse.info.version, s_parse.info.level, s_parse.info.binary ? "binary" : "hex",
                 s_parse.info.dpi, lw_page_name(s_parse.info.page), (long)s_parse.pages);
    } else {
        ESP_LOGW(TAG, "query: no answer (%d)", s_ended);
    }
    ESP_LOGI(TAG, "laser task: %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_use = USE_NONE;
    s_query_at.pages = s_parse.pages;
    if (s_cfg.queried) s_cfg.queried(id, ok, ok ? &s_parse.info : NULL, ok ? &s_query_at : NULL);
}

// ------------------------------------------------------------- the relay
// W-1088: a Mac on one link prints to a printer on another. The Mac's node
// calls these on its link's task (at_relay_t); the laser task opens the
// client and acts on pap_relay_tick.

static void relay_lock(void *ctx)   { (void)ctx; xSemaphoreTake(s_relay_mx, portMAX_DELAY); }
static void relay_unlock(void *ctx) { (void)ctx; xSemaphoreGive(s_relay_mx); }

// The target a node's registration offers: its NBP name, of its kind. Under s_mu.
static int target_of_reg(const lt_node_t *node, int reg)
{
    if (reg < 1 || reg >= LT_REG_MAX || !node->reg) return -1;
    const uint8_t *obj = node->reg[reg].obj, *type = node->reg[reg].type;
    uint8_t lw[NBP_NAME_MAX + 1], iw[NBP_NAME_MAX + 1];
    nbp_name(lw, "LaserWriter");
    nbp_name(iw, "ImageWriter");
    int kind = nbp_match(lw, type) ? LASER_KIND_LW : nbp_match(iw, type) ? LASER_KIND_IW : 0;
    for (int k = 0; kind && k < s_big->n_tg; k++)
        if (s_tg[k].kind == kind && s_tg[k].name[0] == obj[0] &&
            !memcmp(s_tg[k].name + 1, obj + 1, obj[0]))
            return k;
    return -1;
}

static int r_open(int link, const lt_node_t *node, int reg, uint64_t now)
{
    if (!s_started) return 1;
    if (pap_relay_active(&s_big->relay)) return pap_relay_mac_open(&s_big->relay, node, reg, now);
    if (s_use != USE_NONE) return 1;                // a LaserWriter job or a query has the client
    lock();
    int k = target_of_reg(node, reg);
    int id = k >= 0 && lwc_view(&s_core, k, now_ms()).idx >= 0 ? s_tg[k].id : -1;
    unlock();
    if (id < 0) return 1;                           // not in the list, or not heard
    s_relay_id = id;
    s_relay_mac_link = link;
    s_relay_node = node;
    int busy = pap_relay_mac_open(&s_big->relay, node, reg, now);
    if (s_task) xTaskNotifyGive(s_task);
    return busy;
}

static void r_data(const uint8_t *d, size_t n) { pap_relay_mac_data(&s_big->relay, d, n); }
static size_t r_room(void) { return pap_relay_mac_room(&s_big->relay); }
static void r_end(int normal)
{
    if (normal == LT_JOB_EOF) pap_relay_mac_eof(&s_big->relay);   // one job's end (W-1095)
    else pap_relay_mac_end(&s_big->relay, normal);
}
static size_t r_output(uint8_t *buf, size_t max, int *eof, int again)
{
    return pap_relay_mac_output(&s_big->relay, buf, max, eof, again);
}

// SendStatus on a registration: the printer's own words, as it last said them.
static size_t r_status(int link, const lt_node_t *node, int reg, uint8_t *pstr, size_t cap)
{
    (void)link;
    if (!s_started) return 0;
    lock();
    int k = target_of_reg(node, reg);
    size_t n = k >= 0 && s_tg[k].raw_status[0] ? 1 + (size_t)s_tg[k].raw_status[0] : 0;
    if (n > cap) n = 0;
    if (n) memcpy(pstr, s_tg[k].raw_status, n);
    unlock();
    return n;
}

static const at_relay_t RELAY = { r_open, r_data, r_room, r_end, r_output, r_status };

static void abort_mac(void *arg) { lt_node_pap_abort((lt_node_t *)arg, now_ms()); }

// pap_relay_tick's asks, on the laser task.
static void relay_act(uint64_t now)
{
    pap_relay_t *r = &s_big->relay;
    int act = pap_relay_tick(r, now);
    if (act & PR_RELAY_DO_OPEN) {
        lock();
        int k = ti_of(s_relay_id);
        lwc_view_t v = lwc_view(&s_core, k, now);
        lt_route_t route = v.idx >= 0 ? s_core.heard[v.idx].route : (lt_route_t){ 0 };
        int link = v.idx >= 0 ? s_core.heard[v.idx].link : -1;
        unlock();
        xSemaphoreTake(s_client_mx, portMAX_DELAY);
        int ok = k >= 0 && link >= 0 && s_use == USE_NONE && s_cfg.relay && !s_cfg.relay(1);
        if (ok) {
            s_relay_locked = 1;
            s_use = USE_RELAY;
            ride(link);
            lock();
            s_status_id = s_open_id = s_relay_id;
            lwc_job(&s_core, k, 1);
            unlock();
            // As many jobs as the Mac sends on its connection (W-1095).
            if (call(OP_OPEN_MULTI, &route, QUERY_BUSY_MS)) pap_relay_prn_ended(r, PAPC_END_DEAD);
            else ESP_LOGI(TAG, "relay: a Mac's job on the %s for printer %d on the %s",
                          link_name(s_relay_mac_link), s_relay_id, link_name(link));
        } else {
            pap_relay_prn_ended(r, PAPC_END_BUSY);  // the Mac hears busy and asks again
        }
        xSemaphoreGive(s_client_mx);
    }
    if (act & PR_RELAY_DO_CANCEL) {
        xSemaphoreTake(s_client_mx, portMAX_DELAY);
        if (s_use == USE_RELAY) call(OP_CANCEL, NULL, 0);
        xSemaphoreGive(s_client_mx);
    }
    if (act & PR_RELAY_DO_CLOSE) {
        xSemaphoreTake(s_client_mx, portMAX_DELAY);
        if (s_use == USE_RELAY) call(OP_CLOSE, NULL, 0);
        xSemaphoreGive(s_client_mx);
    }
    if (act & PR_RELAY_DO_FINISH) {
        xSemaphoreTake(s_client_mx, portMAX_DELAY);
        if (s_use == USE_RELAY) call(OP_FINISH, NULL, 0);
        xSemaphoreGive(s_client_mx);
    }
    if (act & PR_RELAY_DO_ABORT_MAC) {
        lt_node_t *node = (lt_node_t *)s_relay_node;
        if (s_relay_mac_link == AT_LINK_CABLE) localtalk_locked(abort_mac, node);
        else ethertalk_locked(abort_mac, node);
    }
    if (act & PR_RELAY_DO_DONE) {
        ESP_LOGI(TAG, "relay: done, %lu job%s, %lu bytes to the printer, %lu back to the Mac",
                 (unsigned long)r->jobs, r->jobs == 1 ? "" : "s", (unsigned long)r->to_printer,
                 (unsigned long)r->to_mac);
        if (s_use == USE_RELAY) s_use = USE_NONE;
        lock();
        lwc_job(&s_core, ti_of(s_relay_id), 0);
        snprintf(s_last, sizeof s_last, "relayed, %lu job%s, %lu bytes", (unsigned long)r->jobs,
                 r->jobs == 1 ? "" : "s", (unsigned long)r->to_printer);
        unlock();
        if (s_relay_locked) {
            s_relay_locked = 0;
            if (s_cfg.relay) s_cfg.relay(0);
        }
    }
}

// ------------------------------------------------------------- the task

// The cable always, for the lookups: W-1082 ran it while its card was open,
// and now every look runs on it (review I2); main.c starts it only where U7
// runs TashTalk. The tunnel and the radio where a target lives; otherwise
// they run as their ways in say.
static void links(void)
{
    int want[3] = { 1, 0, 0 };
    lock();
    for (int k = 0; k < s_big->n_tg; k++) {
        int w = s_tg[k].where;
        want[0] |= w == AT_LINK_CABLE;
        want[1] |= w == AT_LINK_TUNNEL;
        want[2] |= w == AT_LINK_RADIO;
    }
    unlock();
    if (!memcmp(want, s_links, sizeof want)) return;
    memcpy(s_links, want, sizeof want);
    if (s_cfg.links) s_cfg.links(want[0], want[1], want[2]);
}

// Each shared target's slot hears its whole state: a fault, a warning, busy
// (review I3). The laser task's.
static void push_states(uint64_t now)
{
    int ready = -1;
    for (int k = 0; k < LWC_TARGETS; k++) {
        lock();
        if (k >= s_big->n_tg) { unlock(); break; }
        tgt_t *g = &s_tg[k];
        lwc_view_t v = lwc_view(&s_core, k, now);
        int slot = g->slot;
        if (slot > 0 && ready < 0) ready = v.online && v.ipp_state != LWC_STOPPED;
        int push = slot > 0 && (g->pushed_slot != slot || v.ipp_state != g->pushed_state ||
                                v.reason != g->pushed_reason || v.busy != g->pushed_busy ||
                                strcmp(v.message, g->pushed_msg));
        char msg[sizeof g->pushed_msg];
        if (push) {
            g->pushed_slot = (int8_t)slot;
            g->pushed_state = (int8_t)v.ipp_state;
            g->pushed_reason = v.reason;
            g->pushed_busy = (int8_t)v.busy;
            snprintf(g->pushed_msg, sizeof g->pushed_msg, "%s", v.message);
            memcpy(msg, g->pushed_msg, sizeof msg);
        }
        unlock();
        if (push) ipp_server_set_state_for(slot, v.reason, msg, v.ipp_state == LWC_STOPPED, v.busy);
    }
    if (ready != s_ready) {
        s_ready = ready;
        if (s_cfg.changed) s_cfg.changed();
    }
}

// A look's lookups, one type at a time (the laser task's).
static void look_step(int *step, uint64_t *at, uint64_t now)
{
    static const char *const types[] = { "AirWriter", "LaserWriter", "ImageWriter" };
    if (!*step || now < *at) return;
    // AirWriters first: a printer another AirWriter relays answers from its node.
    localtalk_lookup(types[(*step - 1) % 3]);
    ethertalk_lookup(types[(*step - 1) % 3]);
    *at = now + (*step == 3 ? LOOK_AGAIN_MS : LOOK_STEP_MS);
    *step = *step < 6 ? *step + 1 : 0;
}

static void laser_task(void *arg)
{
    (void)arg;
    int step = 0;
    uint64_t step_at = 0;
    if (s_cfg.first_look_ms) vTaskDelay(pdMS_TO_TICKS(s_cfg.first_look_ms));
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        uint64_t now = now_ms();

        if (s_open_req) {                          // a job's OpenConn (laser_job_begin)
            s_open_req = 0;
            xSemaphoreTake(s_client_mx, portMAX_DELAY);
            s_open_rc = call(OP_OPEN, &s_open_route, STALL_MS);
            xSemaphoreGive(s_client_mx);
            xSemaphoreGive(s_open_sem);
            ESP_LOGI(TAG, "laser task: %u bytes of stack to spare",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (s_cancel_req) {
            s_cancel_req = 0;
            xSemaphoreTake(s_client_mx, portMAX_DELAY);
            call(OP_CANCEL, NULL, 0);
            xSemaphoreGive(s_client_mx);
        }
        if (s_use == USE_QUERY && s_ended >= 0) query_done();

        lock();
        lwc_want_t w = lwc_tick(&s_core, now);
        lt_route_t status_r = { 0 }, query_r = { 0 };
        int status_link = -1, query_link = -1, query_id = -1, job_link = -1;
        if (w.status_ti != LWC_NONE) {
            int h = lwc_view(&s_core, w.status_ti, now).idx;
            if (h >= 0) {
                status_r = s_core.heard[h].route;
                status_link = s_core.heard[h].link;
                s_status_id = s_core.t[w.status_ti].id;
            }
        }
        if (w.query_ti != LWC_NONE) {
            int h = lwc_view(&s_core, w.query_ti, now).idx;
            if (h >= 0) {
                query_r = s_core.heard[h].route;
                query_link = s_core.heard[h].link;
                query_id = s_core.t[w.query_ti].id;
            }
        }
        if (s_job_id >= 0) {
            int h = lwc_view(&s_core, ti_of(s_job_id), now).idx;
            if (h >= 0) job_link = s_core.heard[h].link;
        }
        uint8_t tell_link = 0, tell_name[NBP_NAME_MAX + 1], tell_type[NBP_NAME_MAX + 1];
        int tell = w.tell != LWC_NONE;
        if (tell) {
            const lw_heard_t *h = &s_core.heard[w.tell];
            tell_link = h->link;
            memcpy(tell_name, h->name, sizeof tell_name);
            nbp_name(tell_type, h->kind == LWC_KIND_IW ? "ImageWriter" : "LaserWriter");
        }
        unlock();

        links();
        if (w.look) {
            step = 1;
            step_at = now;
        }
        look_step(&step, &step_at, now);
        if (tell) {
            char n[3 * NBP_NAME_MAX + 1];
            nbp_utf8(n, sizeof n, tell_name);
            ESP_LOGI(TAG, "heard \"%s\" (%s) on the %s", n,
                     tell_type[1] == 'I' ? "ImageWriter" : "LaserWriter", link_name(tell_link));
            if (s_cfg.found) s_cfg.found(tell_link, tell_name, tell_type);
        }
        if (status_link >= 0) {
            xSemaphoreTake(s_client_mx, portMAX_DELAY);
            // The client moves to ask another only while it has no connection.
            if (s_use == USE_NONE) ride(status_link);
            if (status_link == s_pc_link) call(OP_STATUS, &status_r, 0);
            xSemaphoreGive(s_client_mx);
        }
        if (s_use == USE_NONE && job_link >= 0) {
            xSemaphoreTake(s_client_mx, portMAX_DELAY);
            if (s_use == USE_NONE) ride(job_link);
            xSemaphoreGive(s_client_mx);
        }
        if (query_link >= 0) query_start(query_id, query_link, &query_r);
        relay_act(now);
        push_states(now);
    }
}

esp_err_t laser_start(const laser_cfg_t *cfg)
{
    if (s_started) return ESP_OK;
    s_cfg = *cfg;
    s_mu = xSemaphoreCreateMutex();
    s_client_mx = xSemaphoreCreateMutex();
    s_ended_sem = xSemaphoreCreateBinary();
    s_open_sem = xSemaphoreCreateBinary();
    s_relay_mx = xSemaphoreCreateMutex();
    s_pc = heap_caps_calloc(1, sizeof *s_pc, MALLOC_CAP_SPIRAM);
    if (!s_pc) s_pc = calloc(1, sizeof *s_pc);
    s_big = heap_caps_calloc(1, sizeof *s_big, MALLOC_CAP_SPIRAM);
    if (!s_big) s_big = calloc(1, sizeof *s_big);
    uint8_t *spool = heap_caps_malloc(SPOOL + 1, MALLOC_CAP_SPIRAM);
    uint8_t *m2p = heap_caps_malloc(RELAY_BUF, MALLOC_CAP_SPIRAM);
    uint8_t *p2m = heap_caps_malloc(RELAY_BUF, MALLOC_CAP_SPIRAM);
    if (!s_mu || !s_client_mx || !s_ended_sem || !s_open_sem || !s_relay_mx || !s_pc || !s_big ||
        !spool || !m2p || !p2m)
        return ESP_ERR_NO_MEM;
    pap_relay_init(&s_big->relay, m2p, p2m, RELAY_BUF);
    s_big->relay.lock = relay_lock;
    s_big->relay.unlock = relay_unlock;
    s_spool = xStreamBufferCreateStatic(SPOOL, 1, spool, &s_spool_cb);

    papc_ops_t po = { .read = pc_read, .output = pc_output, .status = pc_status,
                      .opened = pc_opened, .ended = pc_ended, .log = pc_log,
                      .job_eof = pc_job_eof };
    papc_init(s_pc, &po, (uint32_t)esp_timer_get_time() | 1);

    lwc_init(&s_core, now_ms());
    localtalk_set_found(found);
    ethertalk_set_found(found);
    localtalk_set_name_check(name_check);
    ethertalk_set_name_check(name_check);
    at_relay_set(&RELAY);

    // 6 KB: the PAP send path (frames built on the stack, full printf in the
    // log) and mDNS state pushes run here (review I5).
    if (xTaskCreate(laser_task, "laser", 6144, NULL, 3, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    s_started = 1;
    return ESP_OK;
}

int laser_shared(void)
{
    if (!s_started) return 0;
    int any = 0;
    lock();
    for (int k = 0; k < s_big->n_tg; k++) any |= s_tg[k].slot > 0;
    unlock();
    return any;
}

int laser_ready(void) { return s_started ? s_ready : -1; }

int laser_job_active(void) { return s_use == USE_JOB; }

int laser_state(int id, laser_state_t *out)
{
    memset(out, 0, sizeof *out);
    if (!s_started) return -1;
    lock();
    int ti = ti_of(id);
    if (ti >= 0) {
        lwc_view_t v = lwc_view(&s_core, ti, now_ms());
        out->online = (uint8_t)v.online;
        out->ready = v.online && v.ipp_state != LWC_STOPPED;
        out->stopped = v.online && v.ipp_state == LWC_STOPPED;
        out->busy = v.busy;
        out->printing = s_core.t[ti].printing;
    }
    unlock();
    return ti >= 0 ? 0 : -1;
}

// ------------------------------------------------------------- jobs

// The rip's PostScript into the spool, on the pipeline task. Blocks while the
// printer is not pulling (a paused LaserWriter holds the rip, as a paused
// ImageWriter does); gives up at the stall limit, or when the connection is
// gone or the job canceled.
static void spool_write(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    uint32_t waited = 0;
    while (n && !s_job_failed) {
        if (s_job_abort || s_ended >= 0) {
            s_job_failed = 1;
            break;
        }
        int64_t t = esp_timer_get_time();
        size_t k = xStreamBufferSend(s_spool, b, n, pdMS_TO_TICKS(500));
        if (k < n) s_stats.rip_wait_ms += (uint32_t)((esp_timer_get_time() - t) / 1000);
        b += k;
        n -= k;
        if (k) waited = 0;
        else if ((waited += 500) >= STALL_MS) {
            ESP_LOGE(TAG, "the LaserWriter took nothing for %u s; giving up", (unsigned)(STALL_MS / 1000));
            s_job_failed = 1;
        }
    }
    if (s_job_failed) job_pipe_abort();
}

// A job's connection to target id: the PostScript header from the rip's
// writer, or nothing (raw: the caller writes the whole job with spool_write).
static int begin(int id, const char *name, job_pipe_target_t *t, int raw)
{
    if (!s_started) return -1;
    // A query in flight is short: wait for it, as a second job would.
    for (int i = 0; i < 600 && s_use != USE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(100));

    xSemaphoreTake(s_client_mx, portMAX_DELAY);
    lock();
    int ti = ti_of(id);
    tgt_t *g = ti >= 0 ? &s_tg[ti] : NULL;
    lwc_view_t v = lwc_view(&s_core, ti, now_ms());
    lt_route_t r = v.idx >= 0 ? s_core.heard[v.idx].route : (lt_route_t){ 0 };
    int link = v.idx >= 0 ? s_core.heard[v.idx].link : -1;
    // An AirPrint job comes through a slot; the test page goes to any
    // LaserWriter of the list's, shared or not (review I3).
    int ok = g && g->kind == LASER_KIND_LW && (raw || g->slot > 0) && g->have_info && link >= 0;
    ps_cfg_t pc = { .out = spool_write };
    ht_algo_t algo = HT_ATKINSON;
    int gain = 0;
    if (ok) {
        lw_caps_fill(&s_big->job_caps, &g->info);
        pc.binary = g->info.binary;
        pc.paper_h_pt = lw_paper_h_pt(g->info.page);
        if (ht_algo_from_name(g->dither, &algo)) algo = HT_ATKINSON;
        gain = g->gain;
        s_job_id = id;
        s_job_slot = g->slot;
        snprintf(s_job_name, sizeof s_job_name, "%s", g->name_utf8);
        s_status_id = s_open_id = id;
    }
    s_ps_error[0] = '\0';
    unlock();
    pc.title = name;
    if (!ok || s_use != USE_NONE) {
        ESP_LOGW(TAG, "job \"%s\" for printer %d: %s", name, id,
                 !g ? "not in the list" : link < 0 ? "not heard" : !ok ? "not shared" : "busy");
        if (ok) {
            lock();
            s_job_id = s_job_slot = -1;
            unlock();
        }
        xSemaphoreGive(s_client_mx);
        return -1;
    }

    ride(link);
    spool_reset();
    s_job_abort = s_job_failed = s_cancel_req = 0;
    s_ended = -1;
    s_sent = 0;
    s_line_n = 0;
    xSemaphoreTake(s_ended_sem, 0);
    char creator[48];
    snprintf(creator, sizeof creator, "AirWriter %s", s_cfg.version ? s_cfg.version : "");
    pc.creator = creator;
    s_ps = raw ? NULL : ps_new(&pc);
    if (!raw && !s_ps) {
        lock();
        s_job_id = s_job_slot = -1;
        unlock();
        xSemaphoreGive(s_client_mx);
        return -1;
    }
    s_use = USE_JOB;
    memset(&s_stats, 0, sizeof s_stats);
    s_job_t0 = now_ms();
    heap_caps_monitor_local_minimum_free_size_start();
    if (s_ps) {
        ps_stage(s_ps, &s_stage);
        ps_job_begin(s_ps);                        // the header, into the empty spool
    }
    lock();
    lwc_job(&s_core, ti, 1);
    unlock();
    xSemaphoreGive(s_client_mx);
    // OpenConn on the laser task, which owns the client's sends.
    xSemaphoreTake(s_open_sem, 0);
    s_open_route = r;
    s_open_rc = -1;
    s_open_req = 1;
    xTaskNotifyGive(s_task);
    int rc = xSemaphoreTake(s_open_sem, pdMS_TO_TICKS(10000)) == pdTRUE ? s_open_rc : -1;
    if (rc) {
        ESP_LOGE(TAG, "job \"%s\": no connection", name);
        heap_caps_monitor_local_minimum_free_size_stop();
        ps_free(s_ps);
        s_ps = NULL;
        s_use = USE_NONE;
        lock();
        lwc_job(&s_core, ti_of(id), 0);
        s_job_id = s_job_slot = -1;
        unlock();
        return -1;
    }
    ESP_LOGI(TAG, "job \"%s\" to %s, %s", name, s_job_name, raw ? "as it is" : pc.binary ? "binary" : "hex");
    if (raw) return 0;
    // Its own copy, made above: the list may give the targets again while the rip runs.
    t->caps = &s_big->job_caps.caps;
    t->stage = &s_stage;
    t->dither = algo;
    t->gain = gain;
    return 0;
}

int laser_job_begin(int id, const char *name, job_pipe_target_t *t)
{
    return begin(id, name, t, 0);
}

int laser_test_page(int id, const ps_tp_info_t *info)
{
    if (begin(id, "AirWriter test page", NULL, 1)) return 0;
    int ok = !ps_testpage(info, spool_write, NULL);
    return laser_job_end(ok);
}

int laser_job_end(int ok)
{
    if (s_use != USE_JOB) return 0;
    if (ok && !s_job_failed && !s_job_abort) {
        if (s_ps) ps_job_end(s_ps);                // a raw job wrote its own end
    } else {
        s_cancel_req = 1;                          // EOF now: the printer flushes the page
    }
    s_spool_eof = 1;
    if (s_task) xTaskNotifyGive(s_task);
    // The printer prints what is left, paper-out pauses included.
    if (xSemaphoreTake(s_ended_sem, pdMS_TO_TICKS(STALL_MS + 60000)) != pdTRUE) {
        ESP_LOGE(TAG, "the LaserWriter never finished; ending the connection");
        xSemaphoreTake(s_client_mx, portMAX_DELAY);
        call(OP_DOWN, NULL, 0);
        xSemaphoreGive(s_client_mx);
        xSemaphoreTake(s_ended_sem, pdMS_TO_TICKS(1000));
    }
    s_stats.ms = (uint32_t)(now_ms() - s_job_t0);
    s_stats.printer_wait_ms = s_pc->wait_ms;
    s_stats.reads = s_pc->n_reads;
    s_stats.resends = s_pc->n_resends;
    s_stats.ram_low = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_monitor_local_minimum_free_size_stop();
    lock();
    char err[sizeof s_ps_error];
    memcpy(err, s_ps_error, sizeof err);
    int printed = ok && !s_job_failed && s_ended == PAPC_END_OK && !err[0];
    snprintf(s_last, sizeof s_last, "%s, %lu bytes",
             printed ? "printed" : err[0] ? "PostScript error" : s_ended == PAPC_END_CANCELED
             ? "canceled" : "not printed", (unsigned long)s_sent);
    lwc_job(&s_core, ti_of(s_job_id), 0);
    int slot = s_job_slot;
    s_job_id = s_job_slot = -1;
    unlock();
    if (err[0] && slot > 0) ipp_server_job_message_for(slot, err);
    ps_free(s_ps);
    s_ps = NULL;
    s_use = USE_NONE;
    ESP_LOGI(TAG, "job: %s", s_last);
    return printed;
}

void laser_job_cancel(void)
{
    if (s_use != USE_JOB) return;
    s_job_abort = 1;
    s_cancel_req = 1;
    if (s_task) xTaskNotifyGive(s_task);
}

// ------------------------------------------------------------- the web page

static size_t esc(char *out, size_t cap, const char *s)
{
    size_t k = 0;
    for (; *s && k + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
        else if (c < 0x20) k += (size_t)snprintf(out + k, cap - k, "\\u%04x", c);
        else out[k++] = (char)c;
    }
    out[k] = '\0';
    return k;
}

static const char *state_name(const lwc_view_t *v)
{
    return !v->online ? "offline" : v->ipp_state == LWC_STOPPED ? "stopped"
         : v->ipp_state == LWC_PROCESSING ? "printing" : "idle";
}

void laser_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"heard\":[],\"targets\":[]}");
        return;
    }
    uint64_t now = now_ms();
    lock();
    size_t k = (size_t)snprintf(buf, cap, "{\"heard\":[");
    int first = 1;
    for (int i = 0; i < LW_HEARD_MAX && k < cap; i++) {
        const lw_heard_t *h = &s_core.heard[i];
        if (!h->used) continue;
        char name[3 * NBP_NAME_MAX + 1], e_name[200];
        nbp_utf8(name, sizeof name, h->name);
        esc(e_name, sizeof e_name, name);
        k += (size_t)snprintf(buf + k, cap - k,
            "%s{\"name\":\"%s\",\"kind\":\"%s\",\"link\":\"%s\",\"ago_s\":%lu,\"airwriter\":%s}",
            first ? "" : ",", e_name, h->kind == LWC_KIND_IW ? "imagewriter" : "laserwriter",
            link_name(h->link), (unsigned long)((now - h->heard) / 1000), h->aw ? "true" : "false");
        first = 0;
    }
    if (k < cap) k += (size_t)snprintf(buf + k, cap - k, "],\"targets\":[");
    for (int i = 0; i < s_big->n_tg && k < cap; i++) {
        const tgt_t *g = &s_tg[i];
        lwc_view_t v = lwc_view(&s_core, i, now);
        char e_name[200], e_msg[300], e_product[200];
        esc(e_name, sizeof e_name, g->name_utf8);
        esc(e_msg, sizeof e_msg, v.message);
        esc(e_product, sizeof e_product, g->have_info ? g->info.product : "");
        k += (size_t)snprintf(buf + k, cap - k,
            "%s{\"id\":%u,\"name\":\"%s\",\"slot\":%d,\"link\":\"%s\",\"state\":\"%s\","
            "\"reason\":\"%s\",\"message\":\"%s\",\"product\":\"%s\",\"queried\":%s}",
            i ? "," : "", g->id, e_name, g->slot,
            link_name(v.idx >= 0 ? s_core.heard[v.idx].link : g->where), state_name(&v),
            v.reason, e_msg, e_product, s_core.t[i].queried ? "true" : "false");
    }
    if (k < cap) {
        char e_last[100];
        esc(e_last, sizeof e_last, s_last);
        k += (size_t)snprintf(buf + k, cap - k,
            "],\"job\":{\"active\":%s,\"sent\":%lu,\"last\":\"%s\",\"ms\":%lu,"
            "\"rip_wait_ms\":%lu,\"printer_wait_ms\":%lu,\"reads\":%lu,\"resends\":%lu,"
            "\"ram_low\":%lu}}",
            s_use == USE_JOB ? "true" : "false", (unsigned long)s_sent, e_last,
            (unsigned long)s_stats.ms, (unsigned long)s_stats.rip_wait_ms,
            (unsigned long)s_stats.printer_wait_ms, (unsigned long)s_stats.reads,
            (unsigned long)s_stats.resends, (unsigned long)s_stats.ram_low);
    }
    unlock();
    if (k >= cap && cap > 2) snprintf(buf, cap, "{\"heard\":[],\"targets\":[],\"full\":true}");
}

void laser_status_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"targets\":0,\"online\":0,\"job\":false}");
        return;
    }
    uint64_t now = now_ms();
    lock();
    int online = 0;
    for (int i = 0; i < s_big->n_tg; i++) online += lwc_view(&s_core, i, now).online;
    snprintf(buf, cap, "{\"targets\":%d,\"online\":%d,\"job\":%s}", s_big->n_tg, online,
             s_use == USE_JOB ? "true" : "false");
    unlock();
}

// ------------------------------------------------------------- W-1082's NVS

int laser_saved(laser_saved_t *out)
{
    memset(out, 0, sizeof *out);
    out->link = -1;
    snprintf(out->dither, sizeof out->dither, "atkinson");
    nvs_handle_t h;
    if (nvs_open("laser", NVS_READONLY, &h) != ESP_OK) return -1;
    uint8_t v = 0, share = 0;
    nvs_get_u8(h, "share", &share);
    size_t n = sizeof out->name;
    if (nvs_get_blob(h, "name", out->name, &n) != ESP_OK || !n || out->name[0] + 1u != n) share = 0;
    if (nvs_get_u8(h, "link", &v) == ESP_OK) out->link = v == 0xFF ? -1 : v;
    struct { uint8_t ver; lw_info_t info; } b;
    n = sizeof b;
    if (nvs_get_blob(h, "info", &b, &n) == ESP_OK && n == sizeof b && b.ver == INFO_VER) {
        out->info = b.info;
        out->have_info = 1;
    }
    n = sizeof out->dither;
    nvs_get_str(h, "dither", out->dither, &n);
    if (nvs_get_u8(h, "gain", &v) == ESP_OK) out->gain = v;
    nvs_close(h);
    return share ? 0 : -1;
}
