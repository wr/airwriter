// SPDX-License-Identifier: GPL-3.0-or-later
//
// AirWriter application entry point.
//
// M4 scope: join WiFi, advertise over mDNS, answer IPP. A job is accepted and
// counted, not printed -- serial_out and printer_state land with the board.
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "halftone.h"
#include "imagewriter.h"
#include "ipp_server.h"

static const char *TAG = "airwriter";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define WIFI_MAX_RETRY     8

static EventGroupHandle_t s_wifi_events;
static int s_retries;
static char s_hostname[40];

// ---------------------------------------------------------------- raster
// A bring-up check that the raster components run on this silicon, not just
// that they compile. Cheap enough to keep on the boot path.

#define SELFTEST_W 256
#define SELFTEST_H 16

static void count_out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)buf;
    *(size_t *)ctx += n;
}

static void raster_selftest(void)
{
    ht_t h;
    if (ht_init(&h, HT_ATKINSON, SELFTEST_W) != 0) {
        ESP_LOGE(TAG, "raster selftest: ht_init failed");
        return;
    }

    static iw_t w;
    size_t emitted = 0;
    iw_init(&w, SELFTEST_W, count_out, &emitted);
    iw_begin_page(&w);

    uint8_t gray[SELFTEST_W];
    uint8_t bits[(SELFTEST_W + 7) / 8];
    for (uint32_t y = 0; y < SELFTEST_H; y++) {
        for (uint32_t x = 0; x < SELFTEST_W; x++)
            gray[x] = (uint8_t)(x * 255 / (SELFTEST_W - 1));
        ht_row(&h, gray, bits);
        iw_row(&w, bits);
    }

    iw_end_page(&w);
    ht_free(&h);
    ESP_LOGI(TAG, "raster selftest: %ux%u page -> %u printer bytes",
             SELFTEST_W, SELFTEST_H, (unsigned)emitted);
}

// ------------------------------------------------------------- document
// M4 discards the document and reports what arrived. M5 replaces this sink
// with the urf_decode -> resample -> halftone -> imagewriter chain.

typedef struct { size_t bytes; int is_urf; } discard_t;
static discard_t s_doc;

static esp_err_t doc_begin(void *ctx, const char *format, const char *name)
{
    (void)ctx;
    s_doc.bytes = 0;
    s_doc.is_urf = 0;
    ESP_LOGI(TAG, "job \"%s\" format %s", name, format[0] ? format : "(unspecified)");
    return ESP_OK;
}

static esp_err_t doc_data(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    // Confirms the client really sent URF and that the first bytes line up
    // with what the host fixtures decode.
    if (s_doc.bytes == 0 && n >= 8)
        s_doc.is_urf = memcmp(buf, "UNIRAST\0", 8) == 0;
    s_doc.bytes += n;
    return ESP_OK;
}

static void doc_end(void *ctx, int ok)
{
    (void)ctx;
    ESP_LOGI(TAG, "document %s: %u bytes, magic %s",
             ok ? "complete" : "TRUNCATED", (unsigned)s_doc.bytes,
             s_doc.is_urf ? "UNIRAST" : "not URF");
}

// ----------------------------------------------------------------- wifi

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "associated; waiting for dhcp");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_retries < WIFI_MAX_RETRY) {
            s_retries++;
            ESP_LOGW(TAG, "wifi retry %d/%d, reason %d", s_retries, WIFI_MAX_RETRY,
                     d ? d->reason : -1);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

// Credentials come from NVS first so a device can be reprovisioned without a
// rebuild; the Kconfig values are the development fallback. M6 replaces both
// with Improv-over-BLE.
static int load_credentials(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t sl = ssid_cap, pl = pass_cap;
        esp_err_t a = nvs_get_str(h, "ssid", ssid, &sl);
        esp_err_t b = nvs_get_str(h, "pass", pass, &pl);
        nvs_close(h);
        if (a == ESP_OK && b == ESP_OK && ssid[0]) {
            ESP_LOGI(TAG, "wifi credentials from NVS");
            return 1;
        }
    }
    strncpy(ssid, CONFIG_AIRWRITER_WIFI_SSID, ssid_cap - 1);
    strncpy(pass, CONFIG_AIRWRITER_WIFI_PASSWORD, pass_cap - 1);
    return ssid[0] != '\0';
}

static esp_err_t wifi_start(void)
{
    char ssid[33] = {0}, pass[65] = {0};
    if (!load_credentials(ssid, sizeof ssid, pass, sizeof pass)) {
        ESP_LOGE(TAG, "no wifi credentials -- run idf.py menuconfig, AirWriter menu");
        return ESP_ERR_INVALID_STATE;
    }

    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, s_hostname);

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    // WPA3 access points may require hash-to-element rather than the older
    // hunting-and-pecking. The default leaves it unspecified, which can
    // associate and then fail to agree a group key -- the station sits
    // associated and never sees the broadcast DHCP offer.
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // A printer is mains powered and has to answer an mDNS query or an IPP
    // request the moment it arrives, so there is nothing to gain from dozing
    // between beacons -- and some APs drop the unicast DHCP reply to a station
    // that is asleep, which strands the device associated but with no lease.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "joining \"%s\"", ssid);
    for (int waited = 0;; waited += 5) {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                               WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                               pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
        if (bits & WIFI_CONNECTED_BIT) return ESP_OK;
        if (bits & WIFI_FAILED_BIT) return ESP_FAIL;

        // Silence here means associated but no lease, which looks identical to
        // a wrong password from the console. Say which it is.
        esp_netif_ip_info_t ip = {0};
        esp_netif_get_ip_info(netif, &ip);
        ESP_LOGW(TAG, "%ds: no lease yet, netif ip " IPSTR, waited, IP2STR(&ip.ip));
    }
}

// ----------------------------------------------------------------- main

void app_main(void)
{
    ESP_LOGI(TAG, "AirWriter starting, %u bytes free", (unsigned)esp_get_free_heap_size());

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Default hostname imagewriter-XXXX, the last two MAC bytes. SPEC.md 3.2.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_hostname, sizeof s_hostname, "imagewriter-%02x%02x", mac[4], mac[5]);

    raster_selftest();

    if (wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "no network; IPP not started");
        for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
    }

    ipp_server_cfg_t cfg = {
        .queue_name = CONFIG_AIRWRITER_QUEUE_NAME,
        .hostname   = s_hostname,
        .port       = 631,
        .dev_mode   = CONFIG_AIRWRITER_DEV_MODE,
        .sink = { .begin = doc_begin, .data = doc_data, .end = doc_end, .ctx = NULL },
    };
    ESP_ERROR_CHECK(ipp_server_start(&cfg));

    ESP_LOGI(TAG, "ready: %s", ipp_server_uri());
    ESP_LOGI(TAG, "%u bytes free after startup", (unsigned)esp_get_free_heap_size());

    for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
}
