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

esp_err_t settings_load(settings_t *out, const char *mac_suffix)
{
    memset(out, 0, sizeof *out);

    char default_host[SET_HOST_MAX];
    snprintf(default_host, sizeof default_host, "imagewriter-%s", mac_suffix);

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
        nvs_close(h);
    } else {
        strncpy(out->hostname, default_host, sizeof out->hostname - 1);
        strncpy(out->queue, CONFIG_AIRWRITER_QUEUE_NAME, sizeof out->queue - 1);
        strncpy(out->dither, "atkinson", sizeof out->dither - 1);
        out->gain = CONFIG_AIRWRITER_DOT_GAIN;
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
