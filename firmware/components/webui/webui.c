// SPDX-License-Identifier: GPL-3.0-or-later
#include "webui.h"
#include "webui_page.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
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

static esp_err_t status_get(httpd_req_t *req)
{
    static char json[1024];
    json[0] = '\0';
    if (s_cfg.status_json) s_cfg.status_json(s_cfg.ctx, json, sizeof json);
    if (!json[0]) strcpy(json, "{}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t lt_get(httpd_req_t *req)
{
    if (!s_cfg.lt_json) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no localtalk log");
    static char json[4608];
    s_cfg.lt_json(s_cfg.ctx, json, sizeof json);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
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

    uint16_t n = 0;
    if (err == ESP_OK) esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;

    wifi_ap_record_t *aps = n ? calloc(n, sizeof *aps) : NULL;
    if (aps) esp_wifi_scan_get_ap_records(&n, aps);

    // Each network as {s: name, r: rssi, a: 1 if it needs a password}. The UI
    // draws a signal glyph and a lock from these, the way a phone does.
    char out[1400];
    size_t len = 0;
    out[len++] = '[';
    for (uint16_t i = 0; i < n && aps; i++) {
        if (!aps[i].ssid[0]) continue;
        int w = snprintf(out + len, sizeof out - len,
                         "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}",
                         len > 1 ? "," : "", (char *)aps[i].ssid, aps[i].rssi,
                         aps[i].authmode == WIFI_AUTH_OPEN ? 0 : 1);
        if (w < 0 || (size_t)w >= sizeof out - len) break;
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

    // An empty password field means "keep the stored one", so a user editing
    // the network name does not have to retype it.
    if (!pass[0]) {
        settings_t cur;
        settings_load(&cur, "0000");
        strncpy(pass, cur.pass, sizeof pass - 1);
    }

    settings_save_wifi(ssid, pass);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t general_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof body) < 0)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "short body");

    char host[SET_HOST_MAX] = {0}, queue[SET_QUEUE_MAX] = {0}, dither[SET_DITHER_MAX] = {0};
    json_str(body, "host", host, sizeof host);
    json_str(body, "queue", queue, sizeof queue);
    json_str(body, "dither", dither, sizeof dither);

    settings_save_general(host, queue, dither);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// Raw firmware image in the request body. The browser sends a File, so there
// is a Content-Length and no multipart wrapper to unpick.
static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                          "no ota partition");

    ESP_LOGW(TAG, "firmware update: %u bytes into %s",
             (unsigned)req->content_len, part->label);

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin");

    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        esp_ota_abort(ota);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    size_t left = req->content_len, done = 0, next_log = 128 * 1024;
    while (left) {
        int n = httpd_req_recv(req, buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { err = ESP_FAIL; break; }
        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) break;
        left -= (size_t)n;
        done += (size_t)n;
        if (done >= next_log) {
            ESP_LOGI(TAG, "update: %u KB of %u", (unsigned)(done >> 10),
                     (unsigned)(req->content_len >> 10));
            next_log += 128 * 1024;
        }
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "update failed with %u bytes left", (unsigned)left);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "write failed");
    }

    // esp_ota_end verifies the image before anything is made bootable, so a
    // truncated or foreign upload cannot brick the printer.
    err = esp_ota_end(ota);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image rejected");

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot");

    ESP_LOGW(TAG, "update installed, restarting");
    httpd_resp_sendstr(req, "ok");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
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
    hc.max_uri_handlers = 11;
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
        { .uri = "/api/scan",     .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/api/lt",       .method = HTTP_GET,  .handler = lt_get },
        { .uri = "/api/lt/job",   .method = HTTP_GET,  .handler = lt_job_get },
        { .uri = "/api/wifi",     .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/general",  .method = HTTP_POST, .handler = general_post },
        { .uri = "/update",       .method = HTTP_POST, .handler = update_post },
        { .uri = "/api/localtalk/flash", .method = HTTP_POST, .handler = localtalk_flash_post },
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
