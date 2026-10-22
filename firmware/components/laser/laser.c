// SPDX-License-Identifier: GPL-3.0-or-later
// A LaserWriter shared as a second AirPrint printer (W-1082); see laser.h.
//
// Locks, in the only order they are taken: s_client_mx (who uses the PAP
// client, and which link it rides), then a link's own lock (localtalk_locked,
// ethertalk_locked), then s_mu (everything else here). The client's callbacks
// run on a link's task under that link's lock, so they take s_mu alone and
// never call out: what they learn the laser task acts on.
#include "laser.h"
#include "lw_core.h"
#include "lw_query.h"
#include "lw_caps.h"
#include "ps_page.h"
#include "localtalk.h"
#include "ethertalk.h"
#include "pap_client.h"
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

#ifndef CONFIG_AIRWRITER_LASER_UUID_EPOCH
#define CONFIG_AIRWRITER_LASER_UUID_EPOCH 1
#endif
#define STALL_MS   (CONFIG_AIRWRITER_STALL_ABORT * 1000u)
#define SPOOL      (16 * 1024)       // between the rip and the client: 4 SendData answers
#define QUERY_BUSY_MS 60000          // a query waits out a busy printer this long
#define INFO_VER   1

enum { USE_NONE, USE_QUERY, USE_JOB };

static laser_cfg_t       s_cfg;
static SemaphoreHandle_t s_mu, s_client_mx, s_ended_sem;
static TaskHandle_t      s_task;
static int               s_started;

// Under s_mu.
static lwc_t     s_core;
static int       s_shared;
static uint8_t   s_name[NBP_NAME_MAX + 1];        // the shared one's NBP name (Pascal)
static int       s_link = -1;                      // where it was last heard (NVS)
static char      s_name_utf8[3 * NBP_NAME_MAX + 1];
static char      s_uuid[46];
static lw_info_t s_info;
static int       s_have_info;
static lw_caps_t s_caps;
static char      s_dither[12] = "atkinson";
static int       s_gain;
static uint64_t  s_card_at;
static char      s_ps_error[128];
static char      s_last[48] = "";                   // the last job's end, for the card
static int       s_status_idx = -1;                 // the heard[] entry asked last

// The laser task's.
static int       s_advertised, s_readvertise, s_pc_link = -1, s_pushed_state = -1, s_pushed_busy;
static const char *s_pushed_reason;
static char      s_pushed_msg[240];
static uint8_t   s_query_name[NBP_NAME_MAX + 1];    // whom the query in flight asked (review M12)
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
static lw_parse_t s_parse;                          // the query's answers (link task, then ours)
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

// ------------------------------------------------------------- NVS

static void nvs_save_share(void)
{
    nvs_handle_t h;
    if (nvs_open("laser", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "share", (uint8_t)s_shared);
    nvs_set_blob(h, "name", s_name, 1 + (size_t)s_name[0]);
    nvs_set_u8(h, "link", (uint8_t)(s_link < 0 ? 0xFF : s_link));
    if (!s_have_info) nvs_erase_key(h, "info");
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_info(void)
{
    struct { uint8_t ver; lw_info_t info; } b = { INFO_VER, s_info };
    nvs_handle_t h;
    if (nvs_open("laser", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "info", &b, sizeof b);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_screen(void)
{
    nvs_handle_t h;
    if (nvs_open("laser", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "dither", s_dither);
    nvs_set_u8(h, "gain", (uint8_t)s_gain);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("laser", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "share", &v) == ESP_OK) s_shared = v;
    size_t n = sizeof s_name;
    if (nvs_get_blob(h, "name", s_name, &n) != ESP_OK || !n || s_name[0] + 1u != n) s_shared = 0;
    if (nvs_get_u8(h, "link", &v) == ESP_OK) s_link = v == 0xFF ? -1 : v;
    struct { uint8_t ver; lw_info_t info; } b;
    n = sizeof b;
    if (nvs_get_blob(h, "info", &b, &n) == ESP_OK && n == sizeof b && b.ver == INFO_VER) {
        s_info = b.info;
        s_have_info = 1;
    }
    n = sizeof s_dither;
    nvs_get_str(h, "dither", s_dither, &n);
    if (nvs_get_u8(h, "gain", &v) == ESP_OK) s_gain = v;
    nvs_close(h);
}

int laser_shared_saved(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("laser", NVS_READONLY, &h) != ESP_OK) return 0;
    nvs_get_u8(h, "share", &v);
    nvs_close(h);
    return v;
}

// The shared printer's name in UTF-8, its UUID and its caps. Under s_mu.
static void derive(void)
{
    nbp_utf8(s_name_utf8, sizeof s_name_utf8, s_name);
    lw_uuid(s_uuid, s_cfg.mac, CONFIG_AIRWRITER_LASER_UUID_EPOCH, s_name);
    if (s_have_info) lw_caps_fill(&s_caps, &s_info);
}

// ------------------------------------------------------------- the client's links

typedef struct { int op; lt_route_t r; uint32_t busy; int rc; } call_t;
enum { OP_OPEN, OP_STATUS, OP_CANCEL, OP_DOWN };

static void do_call(void *arg)
{
    call_t *c = arg;
    uint64_t now = now_ms();
    switch (c->op) {
    case OP_OPEN:   c->rc = papc_open(s_pc, &c->r, c->busy, now); break;
    case OP_STATUS: papc_status(s_pc, &c->r, now); break;
    case OP_CANCEL: papc_cancel(s_pc, now); break;
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
    return ran < 0 ? -1 : op == OP_OPEN ? c.rc : 0;
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
            xSemaphoreTake(s_mu, portMAX_DELAY);
            if (!s_ps_error[0]) memcpy(s_ps_error, msg, sizeof s_ps_error);
            xSemaphoreGive(s_mu);
        }
    }
}

static void pc_status(void *ctx, const uint8_t *pstr)
{
    (void)ctx;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    // A busy OpenConn's status is the shared printer's; a SendStatus answer
    // is for whichever printer was asked last.
    int idx = s_pc->phase == PAPC_OPENING ? lwc_view(&s_core, now_ms()).idx : s_status_idx;
    lwc_status(&s_core, idx, pstr, now_ms());
    xSemaphoreGive(s_mu);
}

static void pc_ended(void *ctx, papc_end_t how)
{
    (void)ctx;
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
    xSemaphoreTake(s_mu, portMAX_DELAY);
    lwc_found(&s_core, t, (uint8_t)link, r, now_ms());
    xSemaphoreGive(s_mu);
}

// ------------------------------------------------------------- the query

static void query_start(int link, const lt_route_t *r)
{
    static uint8_t q[4096];
    size_t n = lw_query_job(q, sizeof q);
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
    xSemaphoreTake(s_mu, portMAX_DELAY);
    s_status_idx = lwc_view(&s_core, now_ms()).idx;
    memcpy(s_query_name, s_name, sizeof s_query_name);
    xSemaphoreGive(s_mu);
    int rc = call(OP_OPEN, r, QUERY_BUSY_MS);
    xSemaphoreGive(s_client_mx);
    if (rc) {
        s_use = USE_NONE;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        lwc_queried(&s_core, 0, now_ms());
        xSemaphoreGive(s_mu);
        return;
    }
    ESP_LOGI(TAG, "asking the LaserWriter what it is");
}

static void query_done(void)
{
    int ok = s_ended == PAPC_END_OK && s_parse.info.complete;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    // Another LaserWriter shared since it was asked: its answers are not
    // this one's (a new query is already wanted for it).
    if (memcmp(s_query_name, s_name, 1 + (size_t)s_name[0])) {
        xSemaphoreGive(s_mu);
        ESP_LOGW(TAG, "query: answered for a LaserWriter no longer shared; dropped");
        s_use = USE_NONE;
        return;
    }
    if (ok) {
        int changed = !s_have_info || memcmp(&s_info, &s_parse.info, sizeof s_info);
        s_info = s_parse.info;
        s_have_info = 1;
        derive();
        if (changed) s_readvertise = 1;
    }
    lwc_queried(&s_core, ok, now_ms());
    xSemaphoreGive(s_mu);
    if (ok) {
        nvs_save_info();
        ESP_LOGI(TAG, "query: %s, PostScript %s level %u, %s, %u dpi, %s", s_parse.info.product,
                 s_parse.info.version, s_parse.info.level, s_parse.info.binary ? "binary" : "hex",
                 s_parse.info.dpi, lw_page_name(s_parse.info.page));
    } else {
        ESP_LOGW(TAG, "query: no answer (%d)", s_ended);
    }
    ESP_LOGI(TAG, "laser task: %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_use = USE_NONE;
}

// ------------------------------------------------------------- the task

static void advertise(void)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int want = s_shared && s_have_info;
    ipp_printer_cfg_t c = {
        .resource = IPP_RESOURCE_LASER, .name = s_name_utf8, .info = s_name_utf8,
        .uuid = s_uuid, .caps = &s_caps.caps, .icon = "laser", .sink = s_cfg.sink,
    };
    xSemaphoreGive(s_mu);
    if (want && (!s_advertised || s_readvertise)) {
        // Strings and caps are copied under ipp_server's own lock.
        if (ipp_server_set_printer(1, &c) == ESP_OK) {
            s_advertised = 1;
            s_readvertise = 0;
            s_pushed_state = -1;
        }
    } else if (!want && s_advertised) {
        if (ipp_server_set_printer(1, NULL) == ESP_OK) s_advertised = 0;
    }
}

static void links(int shared_link, int card_open)
{
    int want[3] = {
        card_open || (s_shared && shared_link == AT_LINK_CABLE),
        s_shared && shared_link == AT_LINK_TUNNEL,
        s_shared && shared_link == AT_LINK_RADIO,
    };
    if (!memcmp(want, s_links, sizeof want)) return;
    memcpy(s_links, want, sizeof want);
    if (s_cfg.links) s_cfg.links(want[0], want[1], want[2]);
}

static void laser_task(void *arg)
{
    (void)arg;
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

        xSemaphoreTake(s_mu, portMAX_DELAY);
        lwc_want_t w = lwc_tick(&s_core, now);
        lwc_view_t v = lwc_view(&s_core, now);
        int card_open = s_card_at && now - s_card_at < LW_CARD_MS;
        lt_route_t shared_r = { 0 }, status_r = { 0 };
        int shared_link = -1, status_link = -1, last_link = s_link;
        if (v.idx >= 0) {
            shared_r = s_core.heard[v.idx].route;
            shared_link = s_core.heard[v.idx].link;
            if (s_link != shared_link) {
                s_link = shared_link;
                xSemaphoreGive(s_mu);
                nvs_save_share();
                xSemaphoreTake(s_mu, portMAX_DELAY);
            }
        }
        if (w.status_idx != LWC_NONE) {
            status_r = s_core.heard[w.status_idx].route;
            status_link = s_core.heard[w.status_idx].link;
            s_status_idx = w.status_idx;
        }
        xSemaphoreGive(s_mu);

        links(shared_link >= 0 ? shared_link : last_link, card_open);
        if (w.look) {
            localtalk_lookup("LaserWriter");
            ethertalk_lookup("LaserWriter");
        }
        if (status_link >= 0) {
            xSemaphoreTake(s_client_mx, portMAX_DELAY);
            // The client rides the shared printer's link; it moves to ask
            // another only while it has no connection.
            if (s_use == USE_NONE) ride(status_link);
            if (status_link == s_pc_link) call(OP_STATUS, &status_r, 0);
            xSemaphoreGive(s_client_mx);
        }
        if (s_use == USE_NONE && shared_link >= 0) {
            xSemaphoreTake(s_client_mx, portMAX_DELAY);
            if (s_use == USE_NONE && status_link < 0) ride(shared_link);
            xSemaphoreGive(s_client_mx);
        }
        if (w.query && shared_link >= 0) query_start(shared_link, &shared_r);

        advertise();
        // The phone hears the whole state: a fault, a warning, busy (review I3).
        if (s_advertised && (v.ipp_state != s_pushed_state || v.reason != s_pushed_reason ||
                             v.busy != s_pushed_busy || strcmp(v.message, s_pushed_msg))) {
            ipp_server_set_state_for(1, v.reason, v.message, v.ipp_state == LWC_STOPPED, v.busy);
            s_pushed_state = v.ipp_state;
            s_pushed_reason = v.reason;
            s_pushed_busy = v.busy;
            snprintf(s_pushed_msg, sizeof s_pushed_msg, "%s", v.message);
        }
        int ready = !v.shared ? -1 : v.online && v.ipp_state != LWC_STOPPED;
        if (ready != s_ready) {
            s_ready = ready;
            if (s_cfg.changed) s_cfg.changed();
        }
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
    s_pc = heap_caps_calloc(1, sizeof *s_pc, MALLOC_CAP_SPIRAM);
    if (!s_pc) s_pc = calloc(1, sizeof *s_pc);
    uint8_t *spool = heap_caps_malloc(SPOOL + 1, MALLOC_CAP_SPIRAM);
    if (!s_mu || !s_client_mx || !s_ended_sem || !s_open_sem || !s_pc || !spool) return ESP_ERR_NO_MEM;
    s_spool = xStreamBufferCreateStatic(SPOOL, 1, spool, &s_spool_cb);

    papc_ops_t po = { .read = pc_read, .output = pc_output, .status = pc_status,
                      .ended = pc_ended, .log = pc_log };
    papc_init(s_pc, &po, (uint32_t)esp_timer_get_time() | 1);

    lwc_init(&s_core, now_ms());
    nvs_load();
    derive();
    if (s_shared) lwc_share(&s_core, s_name, now_ms());
    localtalk_set_found(found);
    ethertalk_set_found(found);

    // 6 KB: the PAP send path (frames built on the stack, full printf in the
    // log) and mDNS registration run here (review I5).
    if (xTaskCreate(laser_task, "laser", 6144, NULL, 3, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    s_started = 1;
    ESP_LOGI(TAG, "LaserWriters: %s%s", s_shared ? "sharing " : "none shared",
             s_shared ? s_name_utf8 : "");
    return ESP_OK;
}

int laser_shared(void) { return s_started && s_shared; }

int laser_ready(void) { return s_started ? s_ready : -1; }

int laser_job_active(void) { return s_use == USE_JOB; }

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
        size_t k = xStreamBufferSend(s_spool, b, n, pdMS_TO_TICKS(500));
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

int laser_job_begin(const char *name, job_pipe_target_t *t)
{
    if (!s_started || !s_shared || !s_advertised) return -1;
    // A query in flight is short: wait for it, as a second job would.
    for (int i = 0; i < 600 && s_use != USE_NONE; i++) vTaskDelay(pdMS_TO_TICKS(100));

    xSemaphoreTake(s_client_mx, portMAX_DELAY);
    xSemaphoreTake(s_mu, portMAX_DELAY);
    lwc_view_t v = lwc_view(&s_core, now_ms());
    lt_route_t r = v.idx >= 0 ? s_core.heard[v.idx].route : (lt_route_t){ 0 };
    int link = v.idx >= 0 ? s_core.heard[v.idx].link : -1;
    ps_cfg_t pc = { .out = spool_write, .binary = s_info.binary,
                    .paper_h_pt = lw_paper_h_pt(s_info.page), .title = name };
    ht_algo_t algo;
    if (ht_algo_from_name(s_dither, &algo)) algo = HT_ATKINSON;
    int gain = s_gain;
    s_ps_error[0] = '\0';
    s_status_idx = v.idx;
    xSemaphoreGive(s_mu);
    if (s_use != USE_NONE || link < 0) {
        ESP_LOGW(TAG, "job \"%s\": %s", name, link < 0 ? "the LaserWriter is not heard" : "busy");
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
    s_ps = ps_new(&pc);
    if (!s_ps) {
        xSemaphoreGive(s_client_mx);
        return -1;
    }
    s_use = USE_JOB;
    ps_stage(s_ps, &s_stage);
    ps_job_begin(s_ps);                            // the header, into the empty spool
    xSemaphoreTake(s_mu, portMAX_DELAY);
    lwc_job(&s_core, 1);
    xSemaphoreGive(s_mu);
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
        ps_free(s_ps);
        s_ps = NULL;
        s_use = USE_NONE;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        lwc_job(&s_core, 0);
        xSemaphoreGive(s_mu);
        return -1;
    }
    t->caps = &s_caps.caps;
    t->stage = &s_stage;
    t->dither = algo;
    t->gain = gain;
    ESP_LOGI(TAG, "job \"%s\" to %s, %s", name, s_name_utf8, s_info.binary ? "binary" : "hex");
    return 0;
}

int laser_job_end(int ok)
{
    if (s_use != USE_JOB) return 0;
    if (ok && !s_job_failed && !s_job_abort) ps_job_end(s_ps);
    else s_cancel_req = 1;                         // EOF now: the printer flushes the page
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
    xSemaphoreTake(s_mu, portMAX_DELAY);
    char err[sizeof s_ps_error];
    memcpy(err, s_ps_error, sizeof err);
    int printed = ok && !s_job_failed && s_ended == PAPC_END_OK && !err[0];
    snprintf(s_last, sizeof s_last, "%s, %lu bytes",
             printed ? "printed" : err[0] ? "PostScript error" : s_ended == PAPC_END_CANCELED
             ? "canceled" : "not printed", (unsigned long)s_sent);
    lwc_job(&s_core, 0);
    xSemaphoreGive(s_mu);
    if (err[0]) ipp_server_job_message_for(1, err);
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
    return !v->shared ? "none" : !v->online ? "offline" : v->ipp_state == LWC_STOPPED ? "stopped"
         : v->ipp_state == LWC_PROCESSING ? "printing" : "idle";
}

void laser_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"heard\":[],\"shared\":null}");
        return;
    }
    uint64_t now = now_ms();
    xSemaphoreTake(s_mu, portMAX_DELAY);
    lwc_card(&s_core, now);                        // the card is open: look, and keep looking
    s_card_at = now ? now : 1;
    lwc_view_t v = lwc_view(&s_core, now);
    size_t k = (size_t)snprintf(buf, cap, "{\"heard\":[");
    int first = 1;
    for (int i = 0; i < LW_HEARD_MAX && k < cap; i++) {
        const lw_heard_t *h = &s_core.heard[i];
        if (!h->used) continue;
        char name[3 * NBP_NAME_MAX + 1], e_name[200], e_status[200];
        nbp_utf8(name, sizeof name, h->name);
        esc(e_name, sizeof e_name, name);
        esc(e_status, sizeof e_status, h->status);
        k += (size_t)snprintf(buf + k, cap - k,
            "%s{\"name\":\"%s\",\"link\":\"%s\",\"status\":\"%s\",\"ago_s\":%lu,\"shared\":%s}",
            first ? "" : ",", e_name, link_name(h->link), e_status,
            (unsigned long)((now - h->heard) / 1000), i == v.idx ? "true" : "false");
        first = 0;
    }
    if (k >= cap) goto full;
    if (!s_shared) {
        k += (size_t)snprintf(buf + k, cap - k, "],\"shared\":null,\"looking\":true}");
    } else {
        char e_name[200], e_product[200], e_msg[200], e_last[100];
        esc(e_name, sizeof e_name, s_name_utf8);
        esc(e_product, sizeof e_product, s_have_info ? s_info.product : "");
        esc(e_msg, sizeof e_msg, v.message);
        esc(e_last, sizeof e_last, s_last);
        k += (size_t)snprintf(buf + k, cap - k,
            "],\"looking\":true,\"shared\":{\"name\":\"%s\",\"link\":\"%s\",\"online\":%s,"
            "\"advertised\":%s,\"queried\":%s,\"state\":\"%s\",\"reason\":\"%s\",\"message\":\"%s\","
            "\"product\":\"%s\",\"version\":\"%s\",\"level\":%u,\"binary\":%s,\"page\":\"%s\","
            "\"dpi\":%u,\"dither\":\"%s\",\"gain\":%d,\"uuid\":\"%s\"},"
            "\"job\":{\"active\":%s,\"sent\":%lu,\"last\":\"%s\"}}",
            e_name, link_name(s_link), v.online ? "true" : "false",
            s_advertised ? "true" : "false", s_have_info ? "true" : "false", state_name(&v),
            v.reason, e_msg, e_product, s_have_info ? s_info.version : "",
            s_have_info ? s_info.level : 0, s_have_info && s_info.binary ? "true" : "false",
            lw_page_name(s_have_info ? s_info.page : LW_LETTER), s_have_info ? s_info.dpi : 300,
            s_dither, s_gain, s_uuid, s_use == USE_JOB ? "true" : "false",
            (unsigned long)s_sent, e_last);
    }
full:
    xSemaphoreGive(s_mu);
    if (k >= cap && cap > 2) snprintf(buf, cap, "{\"heard\":[],\"shared\":null,\"full\":true}");
    if (s_task) xTaskNotifyGive(s_task);
}

int laser_set(int unshare, const char *share, const char *dither, int gain)
{
    if (!s_started) return 400;
    if (s_use == USE_JOB && (unshare || share)) return 409;
    if (dither) {
        ht_algo_t a;
        if (ht_algo_from_name(dither, &a)) return 400;
    }
    if (gain > 45) return 400;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    int status = 0;
    if (share) {
        int found_i = -1;
        for (int i = 0; i < LW_HEARD_MAX && found_i < 0; i++) {
            char name[3 * NBP_NAME_MAX + 1];
            if (!s_core.heard[i].used) continue;
            nbp_utf8(name, sizeof name, s_core.heard[i].name);
            if (!strcmp(name, share)) found_i = i;
        }
        if (found_i < 0) {
            status = 400;
        } else {
            const uint8_t *pn = s_core.heard[found_i].name;
            int same = s_shared && pn[0] == s_name[0] && !memcmp(pn + 1, s_name + 1, pn[0]);
            memcpy(s_name, pn, 1 + (size_t)pn[0]);
            s_link = s_core.heard[found_i].link;
            if (!same) s_have_info = 0;            // another printer: ask it first
            s_shared = 1;
            derive();
            lwc_share(&s_core, s_name, now_ms());
            s_readvertise = 1;
        }
    } else if (unshare) {
        s_shared = 0;
        lwc_share(&s_core, NULL, now_ms());
    }
    if (!status && dither) snprintf(s_dither, sizeof s_dither, "%s", dither);
    if (!status && gain >= 0) s_gain = gain;
    xSemaphoreGive(s_mu);
    if (status) return status;
    if (share || unshare) nvs_save_share();
    if (dither || gain >= 0) nvs_save_screen();
    if (share || unshare) ESP_LOGI(TAG, "%s", share ? "shared" : "unshared");
    if (s_task) xTaskNotifyGive(s_task);
    if (s_cfg.changed) s_cfg.changed();
    return 0;
}

void laser_status_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"shared\":null}");
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    lwc_view_t v = lwc_view(&s_core, now_ms());
    char e_name[200];
    esc(e_name, sizeof e_name, s_name_utf8);
    if (s_shared)
        snprintf(buf, cap, "{\"shared\":\"%s\",\"state\":\"%s\",\"reason\":\"%s\"}", e_name,
                 state_name(&v), v.reason);
    else
        snprintf(buf, cap, "{\"shared\":null}");
    xSemaphoreGive(s_mu);
}
