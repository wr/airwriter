// SPDX-License-Identifier: GPL-3.0-or-later
#include "webui.h"
#include "esp_heap_caps.h"
#include "webui_page.h"
#include "settings.h"
#include "fw_update.h"
#include "memdiag.h"

#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "led_grid.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "webui";

#define BODY_MAX 512
#define OTA_CHUNK 4096

static httpd_handle_t s_httpd;
static webui_cfg_t    s_cfg;

// Pulls one JSON string value out of a flat object. The bodies this UI posts
// are three fields of its own making, so a full parser would be a library to
// carry for no reason.
static int json_str(const char *body, const char *key, char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    p = strchr(p, '"');
    if (!p) return 0;
    p++;

    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;      // the UI never sends escapes, but
        out[n++] = *p++;                   // a password could contain one
    }
    // Cut short, never in the middle of a character: a name with an emoji
    // in its last bytes would otherwise end in half of one.
    if (*p && *p != '"') {
        size_t k = n;
        while (k && ((unsigned char)out[k - 1] & 0xC0) == 0x80) k--;
        if (k && ((unsigned char)out[k - 1] & 0x80)) {
            unsigned char lead = (unsigned char)out[k - 1];
            size_t want = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
            if (n - (k - 1) < want) n = k - 1;
        }
    }
    out[n] = '\0';
    return 1;
}

static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    size_t len = req->content_len < cap - 1 ? req->content_len : cap - 1;
    size_t got = 0;
    while (got < len) {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (int)got;
}

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, WEBUI_HTML, HTTPD_RESP_USE_STRLEN);
}

// The JSON replies' buffers are in PSRAM, made at their first use, as
// printers_get's and laser_get's: as statics they were 11 KB of the internal
// RAM Wi-Fi buffers in (W-1094). One httpd task, so one buffer each.
static char *psram_buf(char **p, size_t n)
{
    if (!*p) *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return *p;
}

static esp_err_t status_get(httpd_req_t *req)
{
    enum { STATUS_JSON = 2048 };
    static char *buf;
    char *json = psram_buf(&buf, STATUS_JSON);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    json[0] = '\0';
    if (s_cfg.status_json) s_cfg.status_json(s_cfg.ctx, json, STATUS_JSON);
    if (!json[0]) strcpy(json, "{}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// The panel's lights as the board drives them now, for the sidebar's picture
// of the panel (W-1092): 37 dots in chain order (led_map.h), red, green and
// blue each as a little-endian 8.8 fixed-point drive, 222 bytes. The page asks
// a few times a second while it is open. 404 on a board with no panel.
static esp_err_t panel_get(httpd_req_t *req)
{
    uint16_t f[LG_NUM_LEDS][3];
    if (!led_grid_frame(f)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no panel");
    uint8_t b[sizeof f], *p = b;
    for (int i = 0; i < LG_NUM_LEDS; i++)
        for (int c = 0; c < 3; c++) { *p++ = f[i][c] & 0xff; *p++ = f[i][c] >> 8; }
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)b, sizeof b);
}

static esp_err_t lt_get(httpd_req_t *req)
{
    if (!s_cfg.lt_json) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no localtalk log");
    enum { LT_JSON = 4608 };
    static char *buf;
    char *json = psram_buf(&buf, LT_JSON);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    s_cfg.lt_json(s_cfg.ctx, json, LT_JSON);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t lt_echo_post(httpd_req_t *req)
{
    char body[8] = {0};
    int got = req->content_len < sizeof body ? httpd_req_recv(req, body, req->content_len) : -1;
    int node = got > 0 ? atoi(body) : 0;
    if (!s_cfg.lt_echo || node < 1 || node > 254)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body: a node number, 1-254");
    s_cfg.lt_echo(s_cfg.ctx, node);
    return httpd_resp_sendstr(req, "sent");
}

// W-1076: EtherTalk. The zone name is only ever in the settings pair below.
static esp_err_t et_get(httpd_req_t *req)
{
    if (!s_cfg.et_json) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no ethertalk");
    enum { ET_JSON = 4608 };
    static char *buf;
    char *json = psram_buf(&buf, ET_JSON);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    s_cfg.et_json(s_cfg.ctx, json, ET_JSON);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// W-1094: what holds internal RAM, now and at its lowest (memdiag.h). In PSRAM.
static esp_err_t mem_get(httpd_req_t *req)
{
    enum { MEM_JSON = 6144 };
    static char *json;
    if (!json) json = heap_caps_malloc(MEM_JSON, MALLOC_CAP_SPIRAM);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    memdiag_json(json, MEM_JSON);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

// Forgets the low snapshot, so the next job's low is its own.
static esp_err_t mem_reset_post(httpd_req_t *req)
{
    memdiag_reset_low();
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t et_settings_reply(httpd_req_t *req)
{
    int tunnel = 1, radio = 1, infinite = 0;
    char zone[40] = "";
    s_cfg.et_settings_get(s_cfg.ctx, &tunnel, &radio, &infinite, zone, sizeof zone);
    char out[136];
    snprintf(out, sizeof out, "{\"radio\":%s,\"tunnel\":%s,\"infinite\":%s,\"zone\":\"%s\"}",
             radio ? "true" : "false", tunnel ? "true" : "false", infinite ? "true" : "false", zone);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t et_settings_get(httpd_req_t *req)
{
    if (!s_cfg.et_settings_get) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no ethertalk");
    return et_settings_reply(req);
}

static esp_err_t lt_job_get(httpd_req_t *req)
{
    size_t len = 0;
    const uint8_t *job = s_cfg.lt_job ? s_cfg.lt_job(s_cfg.ctx, &len) : NULL;
    if (!job) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no localtalk job");
    httpd_resp_set_type(req, "application/octet-stream");
    return httpd_resp_send(req, (const char *)job, (ssize_t)len);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);      // blocking
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "the scan failed");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;

    wifi_ap_record_t *aps = n ? calloc(n, sizeof *aps) : NULL;
    if (aps) esp_wifi_scan_get_ap_records(&n, aps);

    // Each network as {s: name, r: rssi, a: 1 if it needs a password}. The UI
    // draws a signal glyph and a lock from these, the way a phone does. A
    // network's name is anyone's 32 bytes: quotes and backslashes are escaped,
    // control characters dropped, so one odd name cannot spoil the list.
    char out[1400];
    size_t len = 0;
    out[len++] = '[';
    for (uint16_t i = 0; i < n && aps; i++) {
        if (!aps[i].ssid[0]) continue;
        char name[2 * sizeof aps[i].ssid + 1];
        size_t k = 0;
        for (const unsigned char *c = aps[i].ssid; *c && c < aps[i].ssid + sizeof aps[i].ssid; c++) {
            if (*c < 0x20) continue;
            if (*c == '"' || *c == '\\') name[k++] = '\\';
            name[k++] = (char)*c;
        }
        name[k] = '\0';
        int w = snprintf(out + len, sizeof out - len - 1,      // room for the ]
                         "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}",
                         len > 1 ? "," : "", name, aps[i].rssi,
                         aps[i].authmode == WIFI_AUTH_OPEN ? 0 : 1);
        if (w < 0 || (size_t)w >= sizeof out - len - 1) break;
        len += (size_t)w;
    }
    out[len++] = ']';
    out[len] = '\0';
    free(aps);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));      // let the response actually go out
    esp_restart();
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");

    char ssid[SET_SSID_MAX] = {0}, pass[SET_PASS_MAX] = {0};
    if (!json_str(body, "ssid", ssid, sizeof ssid) || !ssid[0])
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no ssid");
    json_str(body, "pass", pass, sizeof pass);

    // An empty password field means "keep the stored one" when it is the same
    // network, so rejoining it needs no retyping. For any other network it
    // means none: an open network joined with the last one's WPA2 password is
    // never found (IDF then holds out for WPA2), and the board falls back to
    // setup mode.
    if (!pass[0]) {
        settings_t cur;
        settings_load(&cur, "0000");
        if (!strcmp(ssid, cur.ssid)) strncpy(pass, cur.pass, sizeof pass - 1);
    }

    settings_save_wifi(ssid, pass);
    if (s_cfg.captive) settings_set_fresh(1);   // W-1088: the finish sheet awaits on the network
    led_grid_blip(LG_BLIP_SAVED);       // W-1049: taken; the restart follows
    httpd_resp_sendstr(req, "{\"ok\":true}");
    // As fw_update_restart: short of internal RAM, restart from this task
    // rather than not at all (W-1094).
    if (xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL) != pdPASS) reboot_task(NULL);
    return ESP_OK;
}

// One JSON number out of a flat object, or `absent` when the key is not there.
static int json_int(const char *body, const char *key, int absent)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return absent;
    p = strchr(p + strlen(pat), ':');
    if (!p) return absent;
    return atoi(p + 1);
}

static int json_bool(const char *body, const char *key);

static esp_err_t general_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");

    char host[SET_HOST_MAX] = {0}, queue[SET_QUEUE_MAX] = {0}, dither[SET_DITHER_MAX] = {0};
    json_str(body, "host", host, sizeof host);
    // A host name goes into mDNS and, unescaped, into every status reply: the
    // page sends only letters, digits and dashes, and so must anything else.
    for (char *c = host; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-') ||
            (*c == '-' && (c == host || !c[1])))
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "a host name is letters, digits and dashes");
    }
    json_str(body, "queue", queue, sizeof queue);
    json_str(body, "dither", dither, sizeof dither);

    int gain = json_int(body, "gain", -1);
    if (gain > 45) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gain is 0 to 45");
    // 1/144 in, signed: the page offers sixteenths of an inch.
    int top = json_int(body, "top_margin", INT_MIN);
    if (top != INT_MIN && (top < SET_TOP_MIN || top > SET_TOP_MAX))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "top_margin is -216 to 144, in 1/144 in");

    settings_save_general(host, queue, dither, gain);
    if (top != INT_MIN) settings_save_top_margin(top);
    // W-1079: the model, and a name of the user's own ("" goes back to the
    // model's); AirPrint and the AppleTalk port.
    char model[SET_QUEUE_MAX] = {0}, name[SET_QUEUE_MAX] = {0};
    int has_model = json_str(body, "model", model, sizeof model);
    int has_name = json_str(body, "name", name, sizeof name);
    if ((has_model && model[0]) || has_name)
        settings_save_printer(has_model ? model : NULL, has_name ? name : NULL);
    int ap = json_bool(body, "airprint"), lt = json_bool(body, "localtalk");
    if (ap >= 0 || lt >= 0) settings_save_switches(ap, lt);
    if (json_bool(body, "finish") == 1) settings_set_fresh(0);   // W-1088: the sheet closed
    if (s_cfg.general_saved) s_cfg.general_saved(s_cfg.ctx);
    led_grid_blip(LG_BLIP_SAVED);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// A JSON true or false for `key` in a flat object: 1, 0, or -1 when absent.
static int json_bool(const char *body, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    return !strncmp(p, "true", 4) ? 1 : !strncmp(p, "false", 5) ? 0 : -1;
}

// W-1088: the printer list. In PSRAM, as laser_get's: eight records with
// the longest names and products run to about 4 KB.
enum { PRINTERS_JSON = 6144 };

static esp_err_t printers_get(httpd_req_t *req)
{
    if (!s_cfg.printers_json) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no printers");
    static char *json;
    if (!json) json = heap_caps_malloc(PRINTERS_JSON, MALLOC_CAP_SPIRAM);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    s_cfg.printers_json(s_cfg.ctx, json, PRINTERS_JSON);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t printers_post(httpd_req_t *req)
{
    if (!s_cfg.printers_post) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no printers");
    char body[BODY_MAX], err[96];
    if (read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
    int rc = s_cfg.printers_post(s_cfg.ctx, body, err, sizeof err);
    if (rc) {
        httpd_resp_set_status(req, rc == 503 ? "503 Service Unavailable" : rc == 409 ? "409 Conflict"
                              : rc == 502 ? "502 Bad Gateway" : "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, err);
    }
    led_grid_blip(LG_BLIP_SAVED);
    return printers_get(req);
}

static esp_err_t laser_get(httpd_req_t *req)
{
    if (!s_cfg.laser_json) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no laser");
    // In PSRAM: internal RAM is Wi-Fi's (W-1082 bench). One httpd task, so one buffer.
    enum { LASER_JSON = 3072 };
    static char *json;
    if (!json) json = heap_caps_malloc(LASER_JSON, MALLOC_CAP_SPIRAM);
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    s_cfg.laser_json(s_cfg.ctx, json, LASER_JSON);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t laser_post(httpd_req_t *req)
{
    if (!s_cfg.laser_set) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no laser");
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
    char share[100], dither[16];
    int has_share = json_str(body, "share", share, sizeof share);
    int unshare = !has_share && strstr(body, "\"share\":null") != NULL;
    int has_dither = json_str(body, "dither", dither, sizeof dither);
    int rc = s_cfg.laser_set(s_cfg.ctx, unshare, has_share ? share : NULL,
                             has_dither ? dither : NULL, json_int(body, "gain", -1));
    if (rc == 409) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "a job is printing");
    }
    if (rc) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no such LaserWriter or screen");
    led_grid_blip(LG_BLIP_SAVED);
    return laser_get(req);
}

static esp_err_t et_settings_post(httpd_req_t *req)
{
    if (!s_cfg.et_settings_set || !s_cfg.et_settings_get)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no ethertalk");
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
    char zone[64];
    int has_zone = json_str(body, "zone", zone, sizeof zone);
    if (s_cfg.et_settings_set(s_cfg.ctx, json_bool(body, "tunnel"), json_bool(body, "radio"),
                              json_bool(body, "infinite"), has_zone ? zone : NULL))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "zone: 1 to 32 of A-Z a-z 0-9 - _ .");
    led_grid_blip(LG_BLIP_SAVED);
    return et_settings_reply(req);
}

// An update refusal, as the page shows it: 409 for "not now", 400 for an
// image or a request that will never do, 500 for the board's own failure.
static esp_err_t update_err(httpd_req_t *req, esp_err_t err, const char *why)
{
    const char *status = err == ESP_ERR_INVALID_STATE ? "409 Conflict"
                       : err == ESP_ERR_OTA_VALIDATE_FAILED || err == ESP_ERR_INVALID_ARG
                         ? "400 Bad Request" : "500 Internal Server Error";
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, why ? why : "update failed");
}

// Raw firmware image in the request body. The browser sends a File, so there
// is a Content-Length and no multipart wrapper to unpick. fw_update holds the
// job lock from here to the restart, gives way to a print job that arrives
// meanwhile, and installs nothing esp_ota_end refuses: since W-1048, an
// image not signed with the key that signed the running one.
static esp_err_t update_post(httpd_req_t *req)
{
    const char *why = NULL;
    esp_err_t err = fw_update_stream_begin(req->content_len, &why);
    if (err != ESP_OK) return update_err(req, err, why);

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        fw_update_stream_abort();
        return update_err(req, ESP_ERR_NO_MEM, "no memory");
    }
    size_t left = req->content_len;
    int timeouts = 0;
    while (left) {
        int n = httpd_req_recv(req, buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            // The job lock is held: a stalled upload, or a job waiting on
            // one, must not keep it.
            err = fw_update_stream_idle(++timeouts, &why);
            if (err != ESP_OK) { free(buf); return update_err(req, err, why); }
            continue;
        }
        timeouts = 0;
        if (n <= 0) {
            free(buf);
            fw_update_stream_abort();
            ESP_LOGE(TAG, "update: upload cut with %u bytes left", (unsigned)left);
            return ESP_FAIL;            // no one is left to answer
        }
        err = fw_update_stream_write(buf, (size_t)n, &why);
        if (err != ESP_OK) { free(buf); return update_err(req, err, why); }
        left -= (size_t)n;
    }
    free(buf);

    err = fw_update_stream_end(&why);
    if (err != ESP_OK) return update_err(req, err, why);
    httpd_resp_sendstr(req, "ok");
    fw_update_restart();
    return ESP_OK;
}

static esp_err_t update_get(httpd_req_t *req)
{
    char json[512];
    fw_update_json(json, sizeof json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t update_check_post(httpd_req_t *req)
{
    const char *why = NULL;
    esp_err_t err = fw_update_check(&why);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : update_err(req, err, why);
}

// Body {"tag":"v0.1.0"} for a release by name; empty for the newest.
static esp_err_t update_install_post(httpd_req_t *req)
{
    char body[BODY_MAX] = "", tag[32] = "";
    if (req->content_len && read_body(req, body, sizeof body) < 0)
        return update_err(req, ESP_ERR_INVALID_ARG, "short body");
    json_str(body, "tag", tag, sizeof tag);
    const char *why = NULL;
    esp_err_t err = fw_update_install(tag[0] ? tag : NULL, &why);
    return err == ESP_OK ? httpd_resp_sendstr(req, "{\"ok\":true}") : update_err(req, err, why);
}

static esp_err_t update_auto_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) < 0)
        return update_err(req, ESP_ERR_INVALID_ARG, "short body");
    int on = strstr(body, "\"auto\":true") != NULL;
    if (fw_update_set_auto(on) != ESP_OK)
        return update_err(req, ESP_FAIL, "could not save the setting");
    return httpd_resp_sendstr(req, on ? "{\"auto\":true}" : "{\"auto\":false}");
}

// Blocks for the couple of seconds the flash takes. Rare and deliberate, so
// it isn't worth a task and a polling endpoint of its own.
static esp_err_t localtalk_flash_post(httpd_req_t *req)
{
    if (!s_cfg.localtalk_flash) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no localtalk");
        return ESP_FAIL;
    }
    char out[256];
    s_cfg.localtalk_flash(s_cfg.ctx, out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// Blocks for the test's 30-400 ms.
// W-1079: the page's Restart, Erase All Settings and Print Test Page.
static esp_err_t restart_post(httpd_req_t *req)
{
    if (!s_cfg.restart) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no restart");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    s_cfg.restart(s_cfg.ctx);
    return ESP_OK;
}

static esp_err_t erase_post(httpd_req_t *req)
{
    if (!s_cfg.erase) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no erase");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    s_cfg.erase(s_cfg.ctx);
    return ESP_OK;
}

static esp_err_t test_post(httpd_req_t *req)
{
    if (!s_cfg.test_page) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no test page");
    char body[BODY_MAX] = "";
    if (req->content_len && read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");
    int rc = s_cfg.test_page(s_cfg.ctx, json_int(body, "id", -1));
    if (rc == 2) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no printer with a test page");
    if (rc) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "the printer is busy");
    }
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t cable_post(httpd_req_t *req)
{
    if (!s_cfg.cable_test) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no cable test");
        return ESP_FAIL;
    }
    char body[8] = {0};
    if (req->content_len > 0 && req->content_len < sizeof body)
        httpd_req_recv(req, body, req->content_len);
    char out[640];
    s_cfg.cable_test(s_cfg.ctx, !strcmp(body, "both"), out, sizeof out);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// In captive mode every unknown path serves the page, so whatever the phone
// probes for on joining the access point opens the setup screen.
static esp_err_t catch_all(httpd_req_t *req)
{
    if (!s_cfg.captive) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such page");
        return ESP_FAIL;
    }
    return page_get(req);
}

esp_err_t webui_start(const webui_cfg_t *cfg)
{
    s_cfg = *cfg;

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port      = 80;
    hc.ctrl_port        = 32080;
    hc.max_uri_handlers = 32;
    hc.stack_size       = 6144;
    hc.lru_purge_enable = true;
    hc.uri_match_fn     = httpd_uri_match_wildcard;
    hc.recv_wait_timeout = 20;
    hc.send_wait_timeout = 20;

    esp_err_t err = httpd_start(&s_httpd, &hc);
    if (err != ESP_OK) return err;

    static const httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/status",   .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/panel",    .method = HTTP_GET,  .handler = panel_get },
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/lt",       .method = HTTP_GET,  .handler = lt_get },
        { .uri = "/api/lt/job",   .method = HTTP_GET,  .handler = lt_job_get },
        { .uri = "/api/lt/echo",  .method = HTTP_POST, .handler = lt_echo_post },
        { .uri = "/api/et",       .method = HTTP_GET,  .handler = et_get },
        { .uri = "/api/et/settings", .method = HTTP_GET,  .handler = et_settings_get },
        { .uri = "/api/et/settings", .method = HTTP_POST, .handler = et_settings_post },
        { .uri = "/api/mem",      .method = HTTP_GET,  .handler = mem_get },
        { .uri = "/api/mem/reset", .method = HTTP_POST, .handler = mem_reset_post },
        { .uri = "/api/printers", .method = HTTP_GET,  .handler = printers_get },
        { .uri = "/api/printers", .method = HTTP_POST, .handler = printers_post },
        { .uri = "/api/laser",    .method = HTTP_GET,  .handler = laser_get },
        { .uri = "/api/laser",    .method = HTTP_POST, .handler = laser_post },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/general",  .method = HTTP_POST, .handler = general_post },
        { .uri = "/update",       .method = HTTP_POST, .handler = update_post },
        { .uri = "/api/update",   .method = HTTP_GET,  .handler = update_get },
        { .uri = "/api/update/check",   .method = HTTP_POST, .handler = update_check_post },
        { .uri = "/api/update/install", .method = HTTP_POST, .handler = update_install_post },
        { .uri = "/api/update/auto",    .method = HTTP_POST, .handler = update_auto_post },
        { .uri = "/api/localtalk/flash", .method = HTTP_POST, .handler = localtalk_flash_post },
        { .uri = "/api/cable",    .method = HTTP_POST, .handler = cable_post },
        { .uri = "/api/restart",  .method = HTTP_POST, .handler = restart_post },
        { .uri = "/api/erase",    .method = HTTP_POST, .handler = erase_post },
        { .uri = "/api/test",     .method = HTTP_POST, .handler = test_post },
        { .uri = "/*",            .method = HTTP_GET,  .handler = catch_all },
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &routes[i]));

    ESP_LOGI(TAG, "config page on port 80%s", s_cfg.captive ? " (captive)" : "");
    return ESP_OK;
}

void webui_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}
