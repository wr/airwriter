// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NS  = "airwriter";

// The wifi credentials keep their own namespace because main.c read them from
// there before this component existed, and devices in the field may have them.
static const char *NS_WIFI = "wifi";

// W-1049: set by a Wi-Fi save, read once at the next boot. RTC memory is left
// alone by a software restart and lost on a power cut, which is the point:
// only the restart right after setup greets the join.
#define FRESH_MAGIC 0x57494649u     // "WIFI"
static RTC_NOINIT_ATTR uint32_t s_fresh;

static void get_str(nvs_handle_t h, const char *key, char *dst, size_t cap,
                    const char *fallback)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) == ESP_OK && dst[0]) return;
    strncpy(dst, fallback, cap - 1);
    dst[cap - 1] = '\0';
}

// The models the page offers, for a board from before W-1079 that kept only
// a name: one of these was a model picked from the list, anything else a
// name of the user's own on an ImageWriter II.
static const char *const MODELS[] = { "ImageWriter II", "ImageWriter", "ImageWriter LQ" };

static void printer_names(nvs_handle_t h, settings_t *out)
{
    size_t len = sizeof out->model;
    if (nvs_get_str(h, "model", out->model, &len) == ESP_OK && out->model[0]) {
        len = sizeof out->name;
        if (nvs_get_str(h, "name", out->name, &len) != ESP_OK) out->name[0] = '\0';
    } else {
        int known = 0;
        for (size_t i = 0; i < sizeof MODELS / sizeof MODELS[0]; i++)
            known |= !strcmp(out->queue, MODELS[i]);
        strncpy(out->model, known ? out->queue : MODELS[0], sizeof out->model - 1);
        strncpy(out->name, known ? "" : out->queue, sizeof out->name - 1);
    }
    snprintf(out->queue, sizeof out->queue, "%s", out->name[0] ? out->name : out->model);
}

esp_err_t settings_load(settings_t *out, const char *mac_suffix)
{
    memset(out, 0, sizeof *out);

    char default_host[SET_HOST_MAX];
    snprintf(default_host, sizeof default_host, "airwriter-%s", mac_suffix);

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "hostname", out->hostname, sizeof out->hostname, default_host);
        get_str(h, "queue",    out->queue,    sizeof out->queue, CONFIG_AIRWRITER_QUEUE_NAME);
        get_str(h, "dither",   out->dither,   sizeof out->dither, "atkinson");
        uint8_t gn = CONFIG_AIRWRITER_DOT_GAIN;
        if (nvs_get_u8(h, "gain", &gn) == ESP_OK && gn > 45) gn = CONFIG_AIRWRITER_DOT_GAIN;
        out->gain = gn;
        uint8_t au = 0;
        if (nvs_get_u8(h, "autoupd", &au) == ESP_OK) out->auto_update = au ? 1 : 0;
        uint8_t rb = 0;
        if (nvs_get_u8(h, "ribbon", &rb) == ESP_OK && rb <= 2) out->ribbon = rb;
        uint8_t ap = 1, lt = 0xFF;
        nvs_get_u8(h, "airprint", &ap);
        out->airprint = ap ? 1 : 0;
        out->lt_port = nvs_get_u8(h, "lt_port", &lt) == ESP_OK && lt <= 1 ? (int8_t)lt : -1;
        int16_t tm;
        out->top_margin = nvs_get_i16(h, "topmargin", &tm) == ESP_OK && tm >= SET_TOP_MIN &&
                          tm <= SET_TOP_MAX ? tm : CONFIG_AIRWRITER_TOP_MARGIN;
        printer_names(h, out);
        nvs_close(h);
    } else {
        strncpy(out->hostname, default_host, sizeof out->hostname - 1);
        strncpy(out->queue, CONFIG_AIRWRITER_QUEUE_NAME, sizeof out->queue - 1);
        strncpy(out->model, MODELS[0], sizeof out->model - 1);
        if (strcmp(out->queue, out->model)) strncpy(out->name, out->queue, sizeof out->name - 1);
        strncpy(out->dither, "atkinson", sizeof out->dither - 1);
        out->gain = CONFIG_AIRWRITER_DOT_GAIN;
        out->airprint = 1;
        out->lt_port = -1;
        out->top_margin = CONFIG_AIRWRITER_TOP_MARGIN;
    }

    if (nvs_open(NS_WIFI, NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", out->ssid, sizeof out->ssid, CONFIG_AIRWRITER_WIFI_SSID);
        get_str(h, "pass", out->pass, sizeof out->pass, CONFIG_AIRWRITER_WIFI_PASSWORD);
        nvs_close(h);
    } else {
        strncpy(out->ssid, CONFIG_AIRWRITER_WIFI_SSID, sizeof out->ssid - 1);
        strncpy(out->pass, CONFIG_AIRWRITER_WIFI_PASSWORD, sizeof out->pass - 1);
    }
    return ESP_OK;
}

esp_err_t settings_first_boot(const char *host_fmt, const char *mac_suffix, int lt_port)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char host[SET_HOST_MAX];
    size_t len = sizeof host;
    if (nvs_get_str(h, "hostname", host, &len) != ESP_OK || !host[0]) {
        snprintf(host, sizeof host, host_fmt, mac_suffix);
        nvs_set_str(h, "hostname", host);
        ESP_LOGI(TAG, "host name %s", host);
    }
    uint8_t lt;
    if (lt_port >= 0 && nvs_get_u8(h, "lt_port", &lt) != ESP_OK)
        nvs_set_u8(h, "lt_port", lt_port ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "ssid", ssid ? ssid : "");
    nvs_set_str(h, "pass", pass ? pass : "");
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) s_fresh = FRESH_MAGIC;
    ESP_LOGI(TAG, "wifi credentials saved for \"%s\"", ssid ? ssid : "");
    return err;
}

int settings_take_wifi_fresh(void)
{
    int fresh = s_fresh == FRESH_MAGIC;
    s_fresh = 0;
    return fresh;
}

esp_err_t settings_save_general(const char *hostname, const char *queue,
                                const char *dither, int gain)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (hostname && hostname[0]) nvs_set_str(h, "hostname", hostname);
    if (queue    && queue[0])    nvs_set_str(h, "queue",    queue);
    if (dither   && dither[0])   nvs_set_str(h, "dither",   dither);
    if (gain >= 0 && gain <= 45) nvs_set_u8(h, "gain", (uint8_t)gain);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_auto_update(int on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, "autoupd", on ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "auto-update %s", on ? "on" : "off");
    return err;
}

esp_err_t settings_save_printer(const char *model, const char *name)
{
    settings_t cur;
    settings_load(&cur, "0000");
    if (model && model[0]) snprintf(cur.model, sizeof cur.model, "%s", model);
    if (name) snprintf(cur.name, sizeof cur.name, "%s", name);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, "model", cur.model);
    nvs_set_str(h, "name", cur.name);
    nvs_set_str(h, "queue", cur.name[0] ? cur.name : cur.model);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_switches(int airprint, int lt_port)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (airprint >= 0) nvs_set_u8(h, "airprint", airprint ? 1 : 0);
    if (lt_port >= 0) nvs_set_u8(h, "lt_port", lt_port ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_ribbon(uint8_t ribbon)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, "ribbon", ribbon);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_top_margin(int top)
{
    if (top < SET_TOP_MIN || top > SET_TOP_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_i16(h, "topmargin", (int16_t)top);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(NS_WIFI, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "settings erased");
    return ESP_OK;
}
