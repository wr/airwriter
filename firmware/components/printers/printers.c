// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer list on the board (W-1088). printers_core holds the rules; this
// keeps the list in PSRAM under a mutex, stores it in NVS when it changes,
// puts each shared printer on AirPrint as its record says (the oldest port
// record at printer 0, every other shared record, port or AppleTalk, at 1 to
// IPP_PRINTERS-1; W-1116), and gives laser.c the AppleTalk printers.
//
// Locks: s_apply_mx, then s_mu, then laser.c's and ipp_server's own. With s_mu
// held only port_live is called back (live_fill), which takes nothing but
// printer_state's leaf mutex.
#include "printers.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "imagewriter.h"
#include "laser.h"
#include "printer_caps.h"
#include "lw_caps.h"
#include "lt_proto.h"
#include "ipp_req.h"
#include "at_printer.h"

static const char *TAG = "printers";
static const char *NS = "printers";

enum { BLOB_MAX = 8 + sizeof(pr_list_t), SEEN_MAX = 4 + PR_MAX * 9 };

// Every shared record has an AirPrint slot (W-1116, R14): slot 0 is the oldest port record's
// alone and stays empty while that record is not shared (or there is none), so the other
// PR_SHARE_MAX records need slots of their own.
_Static_assert(IPP_PRINTERS >= PR_SHARE_MAX + 1,
               "slot 0 is the oldest port record's alone: a slot for every other shared printer");

// A shared record's AirPrint slot (1 to IPP_PRINTERS-1) as it is wanted, and as it was last
// put there: a LaserWriter's, or (W-1116) a port record's other than the oldest.
typedef struct {
    uint8_t   id, kind, legacy;     // id 0: empty; legacy: answers at ipp/laser (W-1082's)
    uint8_t   model;                // a port record's pr_model_t
    char      name[PR_NAME_MAX];
    char      info[3 * 32 + 1];     // a LaserWriter's NBP name, UTF-8
    char      uuid[46];
    lw_info_t lw;
} slot_t;

// In PSRAM: internal RAM is Wi-Fi's (docs/traps.md).
typedef struct {
    pr_list_t list, scratch;        // scratch: a post works on a copy, all or nothing
    uint8_t   blob[BLOB_MAX], saved[BLOB_MAX];
    size_t    saved_n;
    uint8_t   seen[SEEN_MAX], seen_saved[SEEN_MAX];   // W-1098: key "seen", beside "list"
    size_t    seen_saved_n;
    slot_t    want[IPP_PRINTERS], now[IPP_PRINTERS];  // [0] unused: slot 0 is s_slot0's
    laser_target_t targets[PR_MAX];
    lw_caps_t caps;
} big_t;
static big_t *s_big;
#define s_l (&s_big->list)

static SemaphoreHandle_t s_mu, s_apply_mx;
static printers_cfg_t    s_cfg;
static int               s_started;

// What printer 0 shows, as last put there: the oldest port record (pr_port), at ipp/print
// and (W-1116, R9) at its own ipp/p<id>, so the path a phone cached before the record was
// the oldest keeps routing. Written under s_mu.
typedef struct {
    int     on;
    uint8_t id;
    uint8_t kind;                   // pr_kind_t: an ImageWriter or a StyleWriter
    uint8_t model;
    char    name[PR_NAME_MAX];
    char    uuid[46];
    char    path[16];               // "ipp/p<id>"
} slot0_t;
static slot0_t s_slot0;
static int     s_slot0_known;

static void lock(void)   { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

// ------------------------------------------------------------- NVS

int printers_stored(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    size_t n = 0;
    int ok = nvs_get_blob(h, "list", NULL, &n) == ESP_OK && n > 0;
    nvs_close(h);
    return ok;
}

// A newer firmware's list is in NVS: nothing here writes over it (review I1).
static int s_keep;

// 0 read; -1 none stored; -2 one stored that does not read back here (a
// newer firmware's sets s_keep).
static int load(pr_list_t *l)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t n = 0;
    esp_err_t err = nvs_get_blob(h, "list", NULL, &n);
    if (err == ESP_OK && n > sizeof s_big->saved) {   // bigger than this firmware stores
        nvs_close(h);
        ESP_LOGW(TAG, "the stored list (%u bytes) is a newer firmware's", (unsigned)n);
        s_keep = 1;
        return -2;
    }
    n = sizeof s_big->saved;
    if (err == ESP_OK) err = nvs_get_blob(h, "list", s_big->saved, &n);
    nvs_close(h);
    if (err != ESP_OK) return -1;
    if (pr_load(l, s_big->saved, n)) {
        s_keep = pr_blob_newer(s_big->saved, n);
        ESP_LOGE(TAG, "the stored list (%u bytes) does not read back%s", (unsigned)n,
                 s_keep ? ": a newer firmware's, kept" : "");
        return -2;
    }
    s_big->saved_n = n;
    // W-1098: where each printer last answered, and its page count. Without
    // them (a list from before, or one that won't read) none is told renamed.
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    n = sizeof s_big->seen_saved;
    err = nvs_get_blob(h, "seen", s_big->seen_saved, &n);
    nvs_close(h);
    if (err == ESP_OK && !pr_seen_load(l, s_big->seen_saved, n)) s_big->seen_saved_n = n;
    else if (err == ESP_OK) ESP_LOGW(TAG, "the stored addresses (%u bytes) do not read back", (unsigned)n);
    return 0;
}

// W-1098's addresses, if they differ from what NVS holds. Under s_mu.
static void save_seen(void)
{
    size_t n = pr_seen_save(s_l, s_big->seen, sizeof s_big->seen);
    if (!n || (n == s_big->seen_saved_n && !memcmp(s_big->seen, s_big->seen_saved, n))) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, "seen", s_big->seen, n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not store the addresses: %s", esp_err_to_name(err));
        return;
    }
    memcpy(s_big->seen_saved, s_big->seen, n);
    s_big->seen_saved_n = n;
}

// Stores the list if it differs from what NVS holds. Under s_mu.
static void save(void)
{
    if (s_keep) return;
    save_seen();
    size_t n = pr_save(s_l, s_big->blob, sizeof s_big->blob);
    if (!n || (n == s_big->saved_n && !memcmp(s_big->blob, s_big->saved, n))) return;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_blob(h, "list", s_big->blob, n);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not store the list: %s", esp_err_to_name(err));
        return;
    }
    memcpy(s_big->saved, s_big->blob, n);
    s_big->saved_n = n;
}

// ------------------------------------------------------------- the port's slot

// The printer list's links and at_printer's.
static int at_link_of(int where)
{
    switch (where) {
    case PR_WHERE_CABLE:   return AT_LINK_CABLE;
    case PR_WHERE_NETWORK: return AT_LINK_RADIO;
    case PR_WHERE_TUNNEL:  return AT_LINK_TUNNEL;
    case PR_WHERE_ZONE:    return AT_LINK_ZONE;
    default:               return -1;
    }
}

static pr_where_t where_of(int at_link)
{
    switch (at_link) {
    case AT_LINK_TUNNEL: return PR_WHERE_TUNNEL;
    case AT_LINK_ZONE:   return PR_WHERE_ZONE;
    case AT_LINK_RADIO:  return PR_WHERE_NETWORK;
    default:             return PR_WHERE_CABLE;
    }
}

// The record id and slot a job's sink carries in its ctx (laser_job_begin, main's
// doc_begin, ipp_server_job_done_for).
static void *slot_ctx(int slot, int id) { return (void *)(intptr_t)(slot << 8 | id); }

// A port record's AirPrint capabilities: an ImageWriter's or a StyleWriter's by model; NULL
// for a record that is neither, or a StyleWriter model with none (not shared).
static const printer_caps_t *port_caps(int kind, int model)
{
    if (kind == PR_KIND_STYLEWRITER) return sw_caps(pr_model_to_sw((pr_model_t)model));
    if (kind == PR_KIND_IMAGEWRITER) return printer_caps_imagewriter_model(model);
    return NULL;
}

// Printer 0 as the record says. Under s_mu.
static void slot0_want(slot0_t *w)
{
    memset(w, 0, sizeof *w);
    const pr_rec_t *p = pr_port(s_l);
    if (!p || !p->shared || !port_caps(p->kind, p->model)) return;
    w->on = 1;
    w->id = p->id;
    w->kind = p->kind;
    w->model = p->model;
    snprintf(w->name, sizeof w->name, "%s", p->name);
    snprintf(w->uuid, sizeof w->uuid, "%s", p->uuid);
    snprintf(w->path, sizeof w->path, "ipp/p%u", p->id);
}

static void slot0_cfg(const slot0_t *w, ipp_printer_cfg_t *c)
{
    const printer_caps_t *caps = port_caps(w->kind, w->model);
    *c = (ipp_printer_cfg_t){ .resource = "ipp/print", .legacy_resource = w->path,
                              .name = w->name, .info = caps->make_model, .uuid = w->uuid,
                              .caps = caps,
                              .icon = w->kind == PR_KIND_STYLEWRITER ? "stylewriter" : "icon",
                              .sink = s_cfg.port_sink };
    c->sink.ctx = slot_ctx(0, w->id);
}

const ipp_printer_cfg_t *printers_port_ipp(void)
{
    static ipp_printer_cfg_t c;
    if (!s_started) return NULL;
    xSemaphoreTake(s_apply_mx, portMAX_DELAY);
    lock();
    slot0_want(&s_slot0);
    unlock();
    s_slot0_known = 1;
    if (s_slot0.on) slot0_cfg(&s_slot0, &c);
    int on = s_slot0.on;
    xSemaphoreGive(s_apply_mx);
    return on ? &c : NULL;
}

// Whether a record takes one of slots 1 to IPP_PRINTERS-1: a shared, identified LaserWriter,
// or (W-1116) a shared port record other than the oldest (slot 0's) that has capabilities.
static int wants_slot(const pr_rec_t *r)
{
    if (!r->shared) return 0;
    if (r->where == PR_WHERE_PORT) return r != pr_port(s_l) && port_caps(r->kind, r->model) != NULL;
    return r->kind == PR_KIND_LASERWRITER && r->identified;
}

// What slots 1 to IPP_PRINTERS-1 should hold: a record keeps the slot it has; others take
// the lowest free one. Under s_mu.
static void others_want(void)
{
    slot_t *want = s_big->want, *now = s_big->now;
    memset(want, 0, sizeof s_big->want);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_l->n; i++) {
            const pr_rec_t *r = &s_l->r[i];
            if (!wants_slot(r)) continue;
            int k = 0, placed = 0;
            for (int j = 1; j < IPP_PRINTERS; j++) placed |= want[j].id == r->id;
            if (placed) continue;
            for (int j = 1; j < IPP_PRINTERS && !k; j++)
                if (pass == 0 ? now[j].id == r->id : !want[j].id) k = j;
            if (!k) continue;
            slot_t *w = &want[k];
            w->id = r->id;
            w->kind = r->kind;
            w->model = r->model;
            snprintf(w->name, sizeof w->name, "%s", r->name);
            snprintf(w->uuid, sizeof w->uuid, "%s", r->uuid);
            if (r->kind == PR_KIND_LASERWRITER) {
                w->legacy = r->ipp_laser;
                nbp_utf8(w->info, sizeof w->info, r->nbp);
                w->lw = r->lw;
            }
        }
    }
}

// A record's slot as last put on AirPrint: 0 the oldest port record's, 1.. the others',
// -1 none (not shared, or its change still waits for a job). Under s_mu.
static int slot_of(int id)
{
    if (s_slot0.on && s_slot0.id == id) return 0;
    for (int k = 1; k < IPP_PRINTERS; k++) if (s_big->now[k].id == id) return k;
    return -1;
}

// The targets laser.c serves: every AppleTalk printer, with the slot it has.
// Under s_mu.
static int targets_fill(void)
{
    int n = 0;
    for (int i = 0; i < s_l->n && n < PR_MAX; i++) {
        const pr_rec_t *r = &s_l->r[i];
        if (r->where == PR_WHERE_PORT) continue;
        if (r->kind != PR_KIND_LASERWRITER && r->kind != PR_KIND_AT_IMAGEWRITER) continue;
        laser_target_t *t = &s_big->targets[n++];
        memset(t, 0, sizeof *t);
        t->id = r->id;
        t->kind = r->kind == PR_KIND_LASERWRITER ? LASER_KIND_LW : LASER_KIND_IW;
        t->where = (int8_t)at_link_of(r->where);
        t->slot = (int8_t)slot_of(r->id);
        memcpy(t->name, r->nbp, sizeof t->name);
        snprintf(t->dither, sizeof t->dither, "%s", r->dither);
        t->gain = r->gain;
        t->have_info = r->identified && r->kind == PR_KIND_LASERWRITER;
        t->info = r->lw;
    }
    return n;
}

// Slots 1 to IPP_PRINTERS-1 as wanted. How many changed. Under s_apply_mx.
static int others_apply(void)
{
    int changed = 0;
    for (int k = 1; k < IPP_PRINTERS; k++) {
        lock();
        slot_t w = s_big->want[k];
        int same = !memcmp(&w, &s_big->now[k], sizeof w);
        unlock();
        if (same) continue;
        esp_err_t err;
        if (w.id) {
            char own[16];
            snprintf(own, sizeof own, "ipp/p%u", w.id);
            ipp_printer_cfg_t c = { .resource = own, .name = w.name, .uuid = w.uuid };
            if (w.kind == PR_KIND_LASERWRITER) {
                // W-1082's LaserWriter keeps its path, so a Mac's queue for it still prints.
                if (w.legacy) {
                    c.resource = IPP_RESOURCE_LASER;
                    c.legacy_resource = own;
                }
                lw_caps_fill(&s_big->caps, &w.lw);    // s_apply_mx held: the scratch is ours
                c.caps = &s_big->caps.caps;
                c.info = w.info;
                c.icon = "laser";
                c.sink = s_cfg.at_sink;
            } else {
                // W-1116: a port record, as slot 0's but at its own path.
                c.caps = port_caps(w.kind, w.model);
                c.info = c.caps->make_model;
                c.icon = w.kind == PR_KIND_STYLEWRITER ? "stylewriter" : "icon";
                c.sink = s_cfg.port_sink;
            }
            c.sink.ctx = slot_ctx(k, w.id);
            err = ipp_server_set_printer(k, &c);
        } else {
            err = ipp_server_set_printer(k, NULL);
        }
        if (err == ESP_OK) {
            lock();
            s_big->now[k] = w;
            unlock();
            changed++;
        } else {
            ESP_LOGI(TAG, "printer %d changes after its job (%s)", k, esp_err_to_name(err));
        }
    }
    return changed;
}

void printers_apply(void)
{
    if (!s_started) return;
    xSemaphoreTake(s_apply_mx, portMAX_DELAY);
    slot0_t w;
    int changed = 0;
    lock();
    slot0_want(&w);
    others_want();
    // W-1116: a port record that became the oldest (the one before it was removed) is still
    // in its old slot. It leaves that slot before it takes slot 0, under the same Bonjour
    // name; slot 0 is cleared first, and while a job holds slot 0 nothing moves (the job's
    // end applies again), so the record stays reachable where it is meanwhile. While a job
    // holds its old slot instead, slot 0 is cleared and waits empty for it.
    int moving = s_slot0_known && w.on && slot_of(w.id) > 0;
    unlock();
    if (moving && ipp_server_set_printer(0, NULL) != ESP_OK) {
        ESP_LOGI(TAG, "the port's printer moves to printer 0 after printer 0's job");
    } else {
        if (moving) {
            lock();
            memset(&s_slot0, 0, sizeof s_slot0);
            unlock();
            changed++;
        }
        if (s_slot0_known) changed += others_apply();   // ipp_server is up (printers_port_ipp)
        lock();
        int held = w.on && slot_of(w.id) > 0;      // its old slot's job keeps it there
        unlock();
        if (held) {
            ESP_LOGI(TAG, "the port's printer moves to printer 0 after its job");
        } else if (!s_slot0_known || memcmp(&w, &s_slot0, sizeof w)) {
            ipp_printer_cfg_t c;
            if (w.on) slot0_cfg(&w, &c);
            esp_err_t err = ipp_server_set_printer(0, w.on ? &c : NULL);
            if (err == ESP_OK) {
                lock();
                s_slot0 = w;
                unlock();
                s_slot0_known = 1;
                changed++;
            } else {
                ESP_LOGI(TAG, "the port's printer changes after its job (%s)", esp_err_to_name(err));
            }
        }
    }
    lock();
    int n = targets_fill();
    unlock();
    laser_set_targets(s_big->targets, n);         // s_apply_mx keeps targets[] ours
    xSemaphoreGive(s_apply_mx);
    // A slot put, moved or taken off starts with no state: what each port slot says is
    // pushed again (the slot's change may have waited for a job, with nothing else to push).
    if (changed && s_cfg.placed) s_cfg.placed();
}

// ------------------------------------------------------------- start

static void migrate(pr_legacy_t *o)
{
    char uuid[46];
    ipp_server_legacy_uuid(uuid);
    o->uuid = uuid;
    laser_saved_t *ls = heap_caps_calloc(1, sizeof *ls, MALLOC_CAP_SPIRAM);
    if (ls && laser_saved(ls) == 0) {
        o->laser_shared = 1;
        o->laser_nbp = ls->name;
        o->laser_link = ls->link;
        o->laser_info = ls->have_info ? &ls->info : NULL;
        o->laser_dither = ls->dither;
        o->laser_gain = ls->gain;
    }
    pr_migrate(s_l, o);
    for (int i = 0; i < s_l->n; i++)
        ESP_LOGI(TAG, "carried over: %s \"%s\" (%s), %s", pr_kind_name(s_l->r[i].kind),
                 s_l->r[i].name, pr_where_name(s_l->r[i].where),
                 s_l->r[i].shared ? "shared" : "not shared");
    free(ls);
}

esp_err_t printers_start(const printers_cfg_t *cfg)
{
    s_big = heap_caps_calloc(1, sizeof *s_big, MALLOC_CAP_SPIRAM);
    if (!s_big) s_big = calloc(1, sizeof *s_big);
    s_mu = xSemaphoreCreateMutex();
    s_apply_mx = xSemaphoreCreateMutex();
    if (!s_big || !s_mu || !s_apply_mx) return ESP_ERR_NO_MEM;
    s_cfg = *cfg;

    int rc = load(s_l);
    if (rc == 0) {
        // The MAC and epoch are the board's, whatever the blob said.
        memcpy(s_l->mac, cfg->mac, sizeof s_l->mac);
        s_l->epoch = cfg->epoch;
        ESP_LOGI(TAG, "%u printer%s", s_l->n, s_l->n == 1 ? "" : "s");
    } else {
        pr_init(s_l, cfg->mac, cfg->epoch);
        // A list this firmware can't read: the board's settings from before
        // the list are still in NVS, so its printers come back from them, the
        // port's under its old uuid (a newer firmware's list stays as it is).
        if (cfg->updated_board || rc == -2) {
            pr_legacy_t o = cfg->legacy;
            migrate(&o);
        } else {
            ESP_LOGI(TAG, "no printers yet");
        }
        save();
    }
    s_started = 1;
    return ESP_OK;
}

// ------------------------------------------------------------- the port

static pr_live_t s_live[PR_MAX];

// Every record's live state; the port's from main, to the one record it belongs to
// (pr_port_live_id, W-1116): every other port record is offline. Under s_mu.
static void live_fill(void)
{
    memset(s_live, 0, sizeof s_live);
    int port_id = pr_port_live_id(s_l);
    for (int i = 0; i < s_l->n; i++) {
        const pr_rec_t *r = &s_l->r[i];
        if (r->where == PR_WHERE_PORT) {
            if (r->id == port_id && s_cfg.port_live) s_cfg.port_live(&s_live[i]);
            continue;
        }
        laser_state_t st;
        if (laser_state(r->id, &st)) continue;
        s_live[i] = (pr_live_t){ .present = st.online, .ready = st.ready, .printing = st.printing,
                                 .attention = st.stopped };
    }
}

// Under s_mu. W-1116: the printer on the port is the current record (pr_port_current, whose
// model answered last), not the oldest; a Something else record in the list silences the port
// whatever else is there (PORT_UNKNOWN_SAID), as the design's rule 1 says.
static port_mode_t mode_of(void)
{
    if (pr_port_said_other(s_l)) return PORT_UNKNOWN_SAID;
    const pr_rec_t *p = pr_port_current(s_l);
    if (!p) return PORT_NONE;
    if (p->kind == PR_KIND_IMAGEWRITER) return PORT_IMAGEWRITER;
    if (p->kind == PR_KIND_STYLEWRITER) return PORT_STYLEWRITER;
    return p->found_auto ? PORT_UNKNOWN_FOUND : PORT_UNKNOWN_SAID;
}

// Under s_mu. The record on the port's id (pr_port_current), 0 for none: a change of it, even
// within a family (the LQ for the II), is a change the board follows (its screen and gain, its
// name, what Macs are offered), so it is told (W-1116).
static int cur_id(void)
{
    const pr_rec_t *p = pr_port_current(s_l);
    return p ? p->id : 0;
}

static void after_change(int notify)
{
    save();
    unlock();
    printers_apply();
    if (notify && s_cfg.changed) s_cfg.changed();
}

int printers_port_seen(int iw_model, int ready)
{
    if (!s_started) return -1;
    pr_port_seen_t what = !ready ? PR_PORT_NOTHING
                        : iw_model == IW_MODEL_LQ ? PR_PORT_IWLQ
                        : iw_model == IW_MODEL_II ? PR_PORT_IW2 : PR_PORT_SILENT;
    lock();
    port_mode_t before = mode_of();
    int before_id = cur_id();
    int r = pr_port_seen(s_l, what);
    int moved = mode_of() != before || cur_id() != before_id;
    if (moved) {
        const pr_rec_t *p = pr_port_current(s_l);
        if (p) ESP_LOGI(TAG, "the port: %s \"%s\"", pr_kind_name(p->kind), p->name);
    }
    after_change(moved);
    return r;
}

int printers_port_seen_sw(sw_model_t m)
{
    pr_model_t pm = pr_model_from_sw(m);
    if (!s_started || pm == PR_MODEL_NONE) return -1;
    lock();
    port_mode_t before = mode_of();
    int before_id = cur_id();
    int r = pr_port_seen_sw(s_l, pm);
    int moved = mode_of() != before || cur_id() != before_id;
    if (moved) {
        const pr_rec_t *p = pr_port_current(s_l);
        if (p) ESP_LOGI(TAG, "the port: %s \"%s\"", pr_kind_name(p->kind), p->name);
    }
    after_change(moved);
    return r;
}

void printers_at_seen(int at_link, const uint8_t *name, const uint8_t *type)
{
    if (!s_started) return;
    lock();
    int before = s_l->n;
    int id = pr_at_seen(s_l, where_of(at_link), name, type, 0);
    int added = s_l->n != before;
    if (added) ESP_LOGI(TAG, "found: %s \"%s\"", pr_kind_name(pr_get(s_l, id)->kind),
                        pr_get(s_l, id)->name);
    after_change(added);
}

void printers_at_queried(int id, int ok, const lw_info_t *info, int at_link, uint16_t net,
                         uint8_t node, int32_t pages)
{
    if (!s_started) return;
    lock();
    int renamed = 0;
    if (ok && at_link >= 0) {
        live_fill();
        int to = pr_at_answered(s_l, id, where_of(at_link), net, node, pages, info, s_live);
        if (to >= 0 && to != id) {
            ESP_LOGI(TAG, "printer %d is printer %d renamed: \"%s\", at %u.%u, %ld pages", id, to,
                     pr_get(s_l, to)->name, (unsigned)net, (unsigned)node, (long)pages);
            id = to;
            renamed = 1;
        }
    }
    pr_rec_t *r = pr_get(s_l, id);
    int was = r && r->identified;
    pr_at_queried(s_l, id, ok, info);
    int now = r && pr_get(s_l, id) && pr_get(s_l, id)->identified;
    after_change(renamed || now != was);
}

void printers_offers(int at_link, int on, const char *host, lt_offers_t *o)
{
    memset(o, 0, sizeof *o);
    nbp_name(o->marker, host ? host : "");
    if (!s_started || !on) return;
    int ids[PR_MAX];
    char own[PR_NAME_MAX];
    lock();
    int n = pr_offers(s_l, where_of(at_link), ids, PR_MAX);
    for (int k = 0; k < n; k++) {
        const pr_rec_t *r = pr_get(s_l, ids[k]);
        if (r->where == PR_WHERE_PORT) {
            // The port's one offer, under the name the Chooser and the queue have (W-1116).
            pr_chooser_name(s_l, own, sizeof own);
            nbp_name(o->own, own);
        } else if (o->n_rel < (int)(sizeof o->rel / sizeof o->rel[0])) {
            lt_offer_t *x = &o->rel[o->n_rel++];
            memcpy(x->obj, r->nbp, sizeof x->obj);
            memcpy(x->type, r->nbp_type, sizeof x->type);
        }
    }
    unlock();
}

int printers_test_target(int *port)
{
    *port = 0;
    if (!s_started) return -1;
    lock();
    live_fill();
    int id = pr_test_target(s_l, s_live);
    const pr_rec_t *r = id >= 0 ? pr_get(s_l, id) : NULL;
    // An AppleTalk ImageWriter's page comes with W-1088's last step; until then, none.
    if (r && r->where != PR_WHERE_PORT && r->kind != PR_KIND_LASERWRITER) id = -1;
    *port = r && r->where == PR_WHERE_PORT;
    unlock();
    return id;
}

int printers_test_kind(int id)
{
    if (!s_started) return 0;
    lock();
    const pr_rec_t *r = pr_get(s_l, id);
    int k = !r ? 0 : r->where == PR_WHERE_PORT && r->kind == PR_KIND_IMAGEWRITER ? 1
          : r->where == PR_WHERE_PORT && r->kind == PR_KIND_STYLEWRITER ? 3
          : r->kind == PR_KIND_LASERWRITER && r->identified ? 2 : 0;
    unlock();
    return k;
}

int printers_describe(int id, char *name, size_t ncap, char *product, size_t pcap,
                      char *version, size_t vcap)
{
    if (!s_started) return -1;
    lock();
    const pr_rec_t *r = pr_get(s_l, id);
    if (r) {
        snprintf(name, ncap, "%s", r->name);
        int lw = r->kind == PR_KIND_LASERWRITER;
        snprintf(product, pcap, "%s", lw ? r->lw.product : "");
        snprintf(version, vcap, "%s", lw ? r->lw.version : "");
    }
    unlock();
    return r ? 0 : -1;
}

port_mode_t printers_port_mode(void)
{
    if (!s_started) return PORT_NONE;
    lock();
    port_mode_t m = mode_of();
    unlock();
    return m;
}

int printers_port_is_imagewriter(void)
{
    return printers_port_mode() == PORT_IMAGEWRITER;
}

int printers_port_is_stylewriter(void)
{
    return printers_port_mode() == PORT_STYLEWRITER;
}

int printers_port_shared(void)
{
    if (!s_started) return 0;
    lock();
    const pr_rec_t *p = pr_port_current(s_l);
    int on = p && (p->kind == PR_KIND_IMAGEWRITER || p->kind == PR_KIND_STYLEWRITER) && p->shared;
    unlock();
    return on;
}

pr_model_t printers_port_model(void)      // the port ImageWriter's, else PR_MODEL_NONE
{
    if (!s_started) return PR_MODEL_NONE;
    lock();
    const pr_rec_t *p = pr_port_current(s_l);
    pr_model_t m = p && p->kind == PR_KIND_IMAGEWRITER ? (pr_model_t)p->model : PR_MODEL_NONE;
    unlock();
    return m;
}

sw_model_t printers_port_sw_model(void)
{
    if (!s_started) return SW_NONE;
    lock();
    const pr_rec_t *p = pr_port_current(s_l);
    sw_model_t m = p && p->kind == PR_KIND_STYLEWRITER ? pr_model_to_sw((pr_model_t)p->model) : SW_NONE;
    unlock();
    return m;
}

void printers_port_listed(int *sw, int *iw1, int *sw1)
{
    int any_sw = 0, any_iw1 = 0, any_sw1 = 0;
    if (s_started) {
        lock();
        for (int i = 0; i < s_l->n; i++) {
            const pr_rec_t *r = &s_l->r[i];
            if (r->where != PR_WHERE_PORT) continue;
            any_sw  |= r->kind == PR_KIND_STYLEWRITER;
            any_iw1 |= r->model == PR_MODEL_IW1;
            any_sw1 |= r->model == PR_MODEL_SW1;
        }
        unlock();
    }
    if (sw) *sw = any_sw;
    if (iw1) *iw1 = any_iw1;
    if (sw1) *sw1 = any_sw1;
}

int printers_port_heard(void)
{
    if (!s_started) return PR_HEARD_NONE;
    lock();
    int h = pr_port_heard(s_l);
    unlock();
    return h;
}

void printers_port_answered(int heard)
{
    if (!s_started) return;
    lock();
    pr_port_answered(s_l, heard);            // not stored: nothing to save
    unlock();
}

// W-1116: a job, test page or cleaning for port record `id` goes to another's printer when
// another port record's printer is the one on the port (pr_port_absent): the one that answered
// last and is present. Reads the live state as printers_json does.
int printers_port_absent(int id)
{
    if (!s_started) return 0;
    lock();
    live_fill();
    int a = pr_port_absent(s_l, s_live, id);
    unlock();
    return a;
}

int printers_port_on_is_imagewriter(void)
{
    if (!s_started) return 0;
    lock();
    live_fill();
    int on = pr_port_on(s_l, s_live);
    const pr_rec_t *r = on ? pr_get(s_l, on) : NULL;
    int iw = r && r->kind == PR_KIND_IMAGEWRITER;
    unlock();
    return iw;
}

int printers_record_is_sw(int id)
{
    if (!s_started) return 0;
    lock();
    const pr_rec_t *r = pr_get(s_l, id);
    int sw = r && r->where == PR_WHERE_PORT && r->kind == PR_KIND_STYLEWRITER;
    unlock();
    return sw;
}

pr_model_t printers_record_model(int id)
{
    if (!s_started) return PR_MODEL_NONE;
    lock();
    const pr_rec_t *r = pr_get(s_l, id);
    pr_model_t m = r && r->where == PR_WHERE_PORT ? (pr_model_t)r->model : PR_MODEL_NONE;
    unlock();
    return m;
}

int printers_record_screen(int id, char *dither, size_t cap, int *gain)
{
    if (!s_started) return -1;
    lock();
    const pr_rec_t *r = pr_get(s_l, id);
    if (r && r->where != PR_WHERE_PORT) r = NULL;
    if (r) {
        snprintf(dither, cap, "%s", r->dither);
        *gain = r->gain;
    }
    unlock();
    return r ? 0 : -1;
}

// W-1116: an ImageWriter job's gate (pr_job_gate) on what its own ESC ? and plain questions
// got, never the list's last answer: a printer that answered before the job and was unplugged
// must not end the job as "another printer", nor pass it. The list is read for the record alone.
pr_gate_t printers_job_gate(int id, int heard, pr_model_t heard_model, int line_up)
{
    if (!s_started) return PR_GATE_OTHER;
    lock();
    pr_gate_t g = pr_job_gate(s_l, id, heard, heard_model, line_up);
    unlock();
    return g;
}

void printers_port_set_sw_model(int id, sw_model_t m)
{
    pr_model_t pm = pr_model_from_sw(m);
    if (!s_started || pm == PR_MODEL_NONE) return;
    lock();
    // The full sequence named `m` on the port for the record the person picked (W-1116, `id`):
    // that record takes it, and either way `m` is the printer on the port now (heard set). The
    // person asked, so the page and AirPrint are refreshed whether or not the model changed.
    pr_port_sw_found(s_l, id, pm);
    const pr_rec_t *p = pr_port_current(s_l);
    if (p) ESP_LOGI(TAG, "the port: %s \"%s\"", pr_kind_name(p->kind), p->name);
    after_change(1);
}

void printers_port_name(char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    if (!s_started) return;
    lock();
    const pr_rec_t *p = pr_port_current(s_l);
    if (p) snprintf(out, cap, "%s", p->name);
    unlock();
}

void printers_chooser_name(char *out, size_t cap)
{
    if (!s_started) {
        snprintf(out, cap, "AirWriter");
        return;
    }
    lock();
    pr_chooser_name(s_l, out, cap);
    unlock();
}

int printers_slot_of(int id)
{
    if (!s_started) return -1;
    lock();
    int k = slot_of(id);
    unlock();
    return k;
}

int printers_port_slots(printers_port_slot_t out[], int max)
{
    int n = 0;
    if (!s_started) return 0;
    lock();
    live_fill();
    for (int i = 0; i < s_l->n && n < max; i++) {
        const pr_rec_t *r = &s_l->r[i];
        if (r->where != PR_WHERE_PORT) continue;
        int k = slot_of(r->id);
        if (k < 0) continue;
        printers_port_slot_t *s = &out[n++];
        s->slot = (uint8_t)k;
        s->id = r->id;
        s->kind = r->kind;
        s->model = r->model;
        s->say = (uint8_t)pr_port_say(s_l, s_live, r->id);
    }
    unlock();
    return n;
}

int printers_port_screen(char *dither, size_t cap, int *gain)
{
    if (!s_started) return -1;
    lock();
    const pr_rec_t *p = pr_port_current(s_l);
    if (p) {
        snprintf(dither, cap, "%s", p->dither);
        *gain = p->gain;
    }
    unlock();
    return p ? 0 : -1;
}

void printers_port_set_screen(const char *dither, int gain)
{
    if (!s_started) return;
    lock();
    pr_rec_t *p = (pr_rec_t *)pr_port_current(s_l);
    if (p) {
        if (dither && dither[0]) snprintf(p->dither, sizeof p->dither, "%s", dither);
        if (gain >= 0 && gain <= 45) p->gain = (uint8_t)gain;
    }
    after_change(0);
}

// ------------------------------------------------------------- the page

void printers_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"printers\":[],\"notes\":[],\"max_shared\":%d}", PR_SHARE_MAX);
        return;
    }
    lock();
    live_fill();
    if (!pr_json(s_l, s_live, buf, cap)) snprintf(buf, cap, "{\"error\":\"too long\"}");
    unlock();
}

// W-1099: a LaserWriter's start page is set by a job to it, and its record
// follows the printer's answer (printers_at_queried) before this returns.
static int start_page(int id, int on, char *err, size_t cap)
{
    switch (laser_set_start_page(id, on)) {
    case LASER_SP_OK:
        return 0;
    case LASER_SP_BUSY:
        snprintf(err, cap, "The printer is busy. Try again when it’s done.");
        return 409;
    case LASER_SP_REFUSED:
        snprintf(err, cap, "The printer has a password, so AirWriter can’t change this.");
        return 403;
    default:
        snprintf(err, cap, "Can’t reach the printer. Check that it’s switched on.");
        return 502;
    }
}

// W-1103: one cleaning of a StyleWriter's print head, run by the board. Its
// answer is the status: 0; 409 when this StyleWriter is not the printer on the
// port (another port record's printer is, W-1116) or while a job holds the
// printer; or 502 for a printer that is off or has something to see to (no
// paper, a jam, the cover): the one sentence is true of both.
static int clean_head(int id, char *err, size_t cap)
{
    if (!s_cfg.clean) {
        snprintf(err, cap, "AirWriter can’t clean a print head.");
        return 501;
    }
    // W-1116: this StyleWriter is not the printer on the port -- another port record's printer
    // is. Its cleaning would go to that printer, so it waits until it is plugged in again.
    if (printers_port_absent(id)) {
        char name[PR_NAME_MAX] = "";
        lock();
        const pr_rec_t *r = pr_get(s_l, id);
        if (r) snprintf(name, sizeof name, "%s", r->name);
        unlock();
        snprintf(err, cap, "Connect %s to the printer port first.", name);
        return 409;
    }
    int rc = s_cfg.clean(id);
    if (rc == 409) snprintf(err, cap, "The printer is busy. Try again when it’s done.");
    else if (rc) snprintf(err, cap, "Couldn’t clean the print head. Check that the printer is on and ready.");
    return rc;
}

int printers_post(const char *body, char *err, size_t cap)
{
    if (cap) err[0] = '\0';
    if (!s_started) {
        snprintf(err, cap, "AirWriter is starting up.");
        return 503;
    }
    pr_post_t o;
    lock();
    s_big->scratch = *s_l;
    if (pr_post(&s_big->scratch, body, &o)) {
        unlock();
        snprintf(err, cap, "%s", o.err);
        return 400;
    }
    *s_l = s_big->scratch;
    char dither[sizeof s_l->r[0].dither] = "";
    int gain = -1;
    const pr_rec_t *p = pr_port_current(s_l);
    int port_screen = o.screen_id >= 0 && p && p->id == o.screen_id;
    if (port_screen) {
        snprintf(dither, sizeof dither, "%s", p->dither);
        gain = p->gain;
    }
    after_change(o.changed);
    if (port_screen && s_cfg.port_screen) s_cfg.port_screen(dither, gain);
    if (o.look && s_cfg.look) s_cfg.look();
    if (o.model_id >= 0 && s_cfg.port_model) s_cfg.port_model(o.model_id);
    if (o.confirm_sw_id >= 0 && s_cfg.confirm_sw) s_cfg.confirm_sw(o.confirm_sw_id);
    if (o.start_page >= 0) return start_page(o.start_page_id, o.start_page, err, cap);
    if (o.clean_id >= 0) return clean_head(o.clean_id, err, cap);
    return 0;
}

void printers_status_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"count\":0,\"shared\":0,\"port\":\"none\"}");
        return;
    }
    static const char *const modes[] = { "none", "imagewriter", "unknown", "something_else",
                                         "stylewriter" };
    lock();
    int shared = 0;
    for (int i = 0; i < s_l->n; i++) shared += s_l->r[i].shared;
    snprintf(buf, cap, "{\"count\":%u,\"shared\":%d,\"notes\":%u,\"port\":\"%s\"}", s_l->n,
             shared, s_l->n_notes, modes[mode_of()]);
    unlock();
}
