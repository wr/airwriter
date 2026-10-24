// SPDX-License-Identifier: GPL-3.0-or-later
//
// The printer list on the board (W-1088). printers_core holds the rules; this
// keeps the list in PSRAM under a mutex, stores it in NVS when it changes, and
// puts the port's printer on AirPrint as its record says.
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

static const char *TAG = "printers";
static const char *NS = "printers";

enum { BLOB_MAX = 8 + sizeof(pr_list_t) };

// In PSRAM: internal RAM is Wi-Fi's (docs/traps.md).
typedef struct {
    pr_list_t list, scratch;        // scratch: a post works on a copy, all or nothing
    uint8_t   blob[BLOB_MAX], saved[BLOB_MAX];
    size_t    saved_n;
} big_t;
static big_t *s_big;
#define s_l (&s_big->list)

static SemaphoreHandle_t s_mu, s_apply_mx;
static printers_cfg_t    s_cfg;
static int               s_started;

// What printer 0 shows, as last put there.
typedef struct {
    int     on;
    uint8_t model;
    char    name[PR_NAME_MAX];
    char    uuid[46];
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

static int load(pr_list_t *l)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return -1;
    size_t n = sizeof s_big->saved;
    esp_err_t err = nvs_get_blob(h, "list", s_big->saved, &n);
    nvs_close(h);
    if (err != ESP_OK) return -1;
    if (pr_load(l, s_big->saved, n)) {
        ESP_LOGE(TAG, "the stored list (%u bytes) does not read back", (unsigned)n);
        return -1;
    }
    s_big->saved_n = n;
    return 0;
}

// Stores the list if it differs from what NVS holds. Under s_mu.
static void save(void)
{
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

// Printer 0 as the record says. Under s_mu.
static void slot0_want(slot0_t *w)
{
    memset(w, 0, sizeof *w);
    const pr_rec_t *p = pr_port(s_l);
    if (!p || p->kind != PR_KIND_IMAGEWRITER || !p->shared) return;
    w->on = 1;
    w->model = p->model;
    snprintf(w->name, sizeof w->name, "%s", p->name);
    snprintf(w->uuid, sizeof w->uuid, "%s", p->uuid);
}

static void slot0_cfg(const slot0_t *w, ipp_printer_cfg_t *c)
{
    const printer_caps_t *caps = printer_caps_imagewriter_model(w->model);
    *c = (ipp_printer_cfg_t){ .resource = "ipp/print", .name = w->name,
                              .info = caps->make_model, .uuid = w->uuid, .caps = caps,
                              .icon = "icon", .sink = s_cfg.port_sink };
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
    slot0_cfg(&s_slot0, &c);
    int on = s_slot0.on;
    xSemaphoreGive(s_apply_mx);
    return on ? &c : NULL;
}

void printers_apply(void)
{
    if (!s_started) return;
    xSemaphoreTake(s_apply_mx, portMAX_DELAY);
    slot0_t w;
    lock();
    slot0_want(&w);
    unlock();
    if (!s_slot0_known || memcmp(&w, &s_slot0, sizeof w)) {
        ipp_printer_cfg_t c;
        slot0_cfg(&w, &c);
        esp_err_t err = ipp_server_set_printer(0, w.on ? &c : NULL);
        if (err == ESP_OK) {
            s_slot0 = w;
            s_slot0_known = 1;
        } else {
            ESP_LOGI(TAG, "the port's printer changes after its job (%s)", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_apply_mx);
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

    if (load(s_l) == 0) {
        // The MAC and epoch are the board's, whatever the blob said.
        memcpy(s_l->mac, cfg->mac, sizeof s_l->mac);
        s_l->epoch = cfg->epoch;
        ESP_LOGI(TAG, "%u printer%s", s_l->n, s_l->n == 1 ? "" : "s");
    } else {
        pr_init(s_l, cfg->mac, cfg->epoch);
        if (cfg->updated_board) {
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

// Every record's live state; the port's from main. Under s_mu.
static void live_fill(void)
{
    memset(s_live, 0, sizeof s_live);
    const pr_rec_t *p = pr_port(s_l);
    if (p && s_cfg.port_live) s_cfg.port_live(&s_live[pr_index(s_l, p->id)]);
}

// Under s_mu.
static port_mode_t mode_of(void)
{
    const pr_rec_t *p = pr_port(s_l);
    if (!p) return PORT_NONE;
    if (p->kind == PR_KIND_IMAGEWRITER) return PORT_IMAGEWRITER;
    return p->found_auto ? PORT_UNKNOWN_FOUND : PORT_UNKNOWN_SAID;
}

static void after_change(int notify)
{
    save();
    unlock();
    printers_apply();
    if (notify && s_cfg.changed) s_cfg.changed();
}

void printers_port_seen(int iw_model, int ready)
{
    if (!s_started) return;
    pr_port_seen_t what = !ready ? PR_PORT_NOTHING
                        : iw_model == IW_MODEL_LQ ? PR_PORT_IWLQ
                        : iw_model == IW_MODEL_II ? PR_PORT_IW2 : PR_PORT_SILENT;
    lock();
    port_mode_t before = mode_of();
    pr_port_seen(s_l, what);
    int moved = mode_of() != before;
    if (moved) {
        const pr_rec_t *p = pr_port(s_l);
        ESP_LOGI(TAG, "the port: %s \"%s\"", pr_kind_name(p->kind), p->name);
    }
    after_change(moved);
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

int printers_port_shared(void)
{
    if (!s_started) return 0;
    lock();
    const pr_rec_t *p = pr_port(s_l);
    int on = p && p->kind == PR_KIND_IMAGEWRITER && p->shared;
    unlock();
    return on;
}

pr_model_t printers_port_model(void)
{
    if (!s_started) return PR_MODEL_NONE;
    lock();
    const pr_rec_t *p = pr_port(s_l);
    pr_model_t m = p && p->kind == PR_KIND_IMAGEWRITER ? (pr_model_t)p->model : PR_MODEL_NONE;
    unlock();
    return m;
}

void printers_port_name(char *out, size_t cap)
{
    if (cap) out[0] = '\0';
    if (!s_started) return;
    lock();
    const pr_rec_t *p = pr_port(s_l);
    if (p) snprintf(out, cap, "%s", p->name);
    unlock();
}

int printers_port_screen(char *dither, size_t cap, int *gain)
{
    if (!s_started) return -1;
    lock();
    const pr_rec_t *p = pr_port(s_l);
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
    pr_rec_t *p = (pr_rec_t *)pr_port(s_l);
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
    const pr_rec_t *p = pr_port(s_l);
    int port_screen = o.screen_id >= 0 && p && p->id == o.screen_id;
    if (port_screen) {
        snprintf(dither, sizeof dither, "%s", p->dither);
        gain = p->gain;
    }
    after_change(o.changed);
    if (port_screen && s_cfg.port_screen) s_cfg.port_screen(dither, gain);
    if (o.look && s_cfg.look) s_cfg.look();
    return 0;
}

void printers_status_json(char *buf, size_t cap)
{
    if (!s_started) {
        snprintf(buf, cap, "{\"count\":0,\"shared\":0,\"port\":\"none\"}");
        return;
    }
    static const char *const modes[] = { "none", "imagewriter", "unknown", "something_else" };
    lock();
    int shared = 0;
    for (int i = 0; i < s_l->n; i++) shared += s_l->r[i].shared;
    snprintf(buf, cap, "{\"count\":%u,\"shared\":%d,\"notes\":%u,\"port\":\"%s\"}", s_l->n,
             shared, s_l->n_notes, modes[mode_of()]);
    unlock();
}
