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
#include "esp_timer.h"
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
#include "job_pipe.h"
#include "serial_out.h"
#include "settings.h"
#include "webui.h"
#include "net_provision.h"
#include "esp_app_desc.h"

static const char *TAG = "airwriter";

// A Kconfig bool that is off defines nothing at all, so every one of these
// needs a fallback rather than a bare CONFIG_ reference.
#ifdef CONFIG_AIRWRITER_INVERT_TX
#define AW_INVERT_TX 1
#else
#define AW_INVERT_TX 0
#endif
#ifdef CONFIG_AIRWRITER_INVERT_RX
#define AW_INVERT_RX 1
#else
#define AW_INVERT_RX 0
#endif
#ifdef CONFIG_AIRWRITER_UART_LOOPBACK
#define AW_LOOPBACK 1
#else
#define AW_LOOPBACK 0
#endif
#ifdef CONFIG_AIRWRITER_DEV_MODE
#define AW_DEV_MODE 1
#else
#define AW_DEV_MODE 0
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT    BIT1
#define WIFI_MAX_RETRY     8

static EventGroupHandle_t s_wifi_events;
static int s_retries;
static settings_t s_set;
static char s_ip[16] = "0.0.0.0";
static int  s_ap_mode;

// Last job, for the status page.
static char   s_last_job[64];
static size_t s_last_pages, s_last_bytes;

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
// The document goes straight into the raster pipeline. What comes out is the
// ImageWriter byte stream -- the exact bytes serial_out will put on the wire
// once there is a board to put them on. Until then they are checksummed, which
// is enough to compare the device against tools/rip byte for byte.

// CRC-32/ISO-HDLC, computed without a table: 256 entries of flash is not worth
// it at these rates, and this runs once per output byte, not per pixel.
static uint32_t crc32_up(uint32_t crc, const uint8_t *buf, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint32_t s_stream_crc;

// The printer stream: checksummed so the device can be compared with tools/rip
// byte for byte, then put on the wire.
static void printer_out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    s_stream_crc = crc32_up(s_stream_crc, buf, n);
    serial_out_write(buf, n);
}

// HSKo has been low too long. SPEC.md 3.9: that becomes a real alert on the
// phone rather than a job that quietly never finishes.
static void printer_state_changed(void *ctx, serial_out_state_t st)
{
    (void)ctx;
    ipp_server_set_stopped(st == SERIAL_OUT_BLOCKED ? "media-empty" : NULL);
}

static esp_err_t doc_begin(void *ctx, const char *format, const char *name)
{
    (void)ctx;
    ESP_LOGI(TAG, "job \"%s\" format %s", name, format[0] ? format : "(unspecified)");
    strncpy(s_last_job, name && name[0] ? name : "Untitled", sizeof s_last_job - 1);
    s_last_job[sizeof s_last_job - 1] = '\0';
    s_stream_crc = 0;
    serial_out_loopback_reset();
    return job_pipe_begin(name);
}

static esp_err_t doc_data(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    return job_pipe_data(buf, n);
}

static void doc_cancel(void *ctx)
{
    (void)ctx;
    job_pipe_abort();
}

static void doc_end(void *ctx, int ok)
{
    (void)ctx;
    job_pipe_stats_t st;
    job_pipe_end(ok, &st);

    if (st.rc != 0 || !ok) {
        ESP_LOGE(TAG, "job failed: %u bytes in, rc %d%s",
                 (unsigned)st.doc_bytes, st.rc, ok ? "" : ", transfer truncated");
        return;
    }

    s_last_pages = (size_t)st.pages;
    s_last_bytes = st.printer_bytes;
    ESP_LOGI(TAG, "ripped %d page%s, %ux%u dots, %u bytes in -> %u printer bytes in %ums",
             st.pages, st.pages == 1 ? "" : "s", (unsigned)st.dst_w, (unsigned)st.dst_h,
             (unsigned)st.doc_bytes, (unsigned)st.printer_bytes, (unsigned)st.ms);
    ESP_LOGI(TAG, "printer stream crc32 %08x", (unsigned)s_stream_crc);

    // Let the last byte clear the shift register before calling it printed.
    serial_out_flush(120000);

    if (AW_LOOPBACK) {
        size_t got; uint32_t crc;
        serial_out_loopback_wait(st.printer_bytes, 3000);
        serial_out_loopback_stats(&got, &crc);
        if (got == st.printer_bytes && crc == s_stream_crc)
            ESP_LOGI(TAG, "uart loopback: %u bytes returned intact, crc32 %08x",
                     (unsigned)got, (unsigned)crc);
        else
            ESP_LOGE(TAG, "uart loopback MISMATCH: sent %u/%08x, got %u/%08x",
                     (unsigned)st.printer_bytes, (unsigned)s_stream_crc,
                     (unsigned)got, (unsigned)crc);
    }

    // At 9600 baud, 8N1, one byte costs 10 bit times.
    ESP_LOGI(TAG, "that is %u seconds of printing at 9600 baud",
             (unsigned)(st.printer_bytes * 10 / 9600));
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
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, s_set.hostname);

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s_set.ssid, sizeof wc.sta.ssid);
    strncpy((char *)wc.sta.password, s_set.pass, sizeof wc.sta.password);
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

    // Bounded, because associating and never getting a lease is a real state
    // -- a network that admits the device and then drops its traffic looks
    // exactly like this, and waiting forever means the setup portal never
    // appears and the printer cannot be recovered without a cable.
    ESP_LOGI(TAG, "joining \"%s\"", s_set.ssid);
    for (int waited = 0; waited < 60; waited += 5) {
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

    ESP_LOGE(TAG, "joined \"%s\" but never got an address", s_set.ssid);
    return ESP_FAIL;
}

// --------------------------------------------------------------- status
// What the config page shows. Kept here rather than in webui because it is the
// application that knows these things, not the HTTP layer.

static void status_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    const esp_app_desc_t *app = esp_app_get_description();
    int blocked = serial_out_state() == SERIAL_OUT_BLOCKED;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    char job[96];
    if (s_last_job[0])
        snprintf(job, sizeof job, "%s (%u page%s)", s_last_job,
                 (unsigned)s_last_pages, s_last_pages == 1 ? "" : "s");
    else
        snprintf(job, sizeof job, "none yet");

    snprintf(buf, cap,
        "{\"state\":\"%s\",\"stopped\":%s,\"job\":\"%s\",\"queue\":\"%s\","
        "\"host\":\"%s.local\",\"ip\":\"%s\",\"ssid\":\"%s\",\"dither\":\"%s\","
        "\"uptime\":\"%uh %02um\",\"version\":\"%s\",\"heap\":\"%u KB\","
        "\"model\":\"Apple ImageWriter II\"}",
        s_ap_mode ? "Waiting for setup" : blocked ? "Out of paper" : "Ready",
        blocked ? "true" : "false",
        job, s_set.queue, s_set.hostname, s_ip, s_set.ssid, s_set.dither,
        (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
        app->version, (unsigned)(esp_get_free_heap_size() / 1024));
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
    char suffix[8];
    snprintf(suffix, sizeof suffix, "%02x%02x", mac[4], mac[5]);
    settings_load(&s_set, suffix);

    raster_selftest();

    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;

    serial_out_cfg_t ser = {
        .uart_num        = CONFIG_AIRWRITER_UART_NUM,
        .tx_pin          = CONFIG_AIRWRITER_TX_PIN,
        .rx_pin          = CONFIG_AIRWRITER_RX_PIN,
        .cts_pin         = CONFIG_AIRWRITER_CTS_PIN,
        .baud            = CONFIG_AIRWRITER_BAUD,
        .invert_tx       = AW_INVERT_TX,
        .invert_rx       = AW_INVERT_RX,
        .stall_timeout_s = CONFIG_AIRWRITER_STALL_TIMEOUT,
        .on_state        = printer_state_changed,
        .loopback        = AW_LOOPBACK,
    };
    ESP_ERROR_CHECK(serial_out_init(&ser));

    job_pipe_cfg_t pipe = { .out = printer_out, .dither = algo };
    ESP_ERROR_CHECK(job_pipe_init(&pipe));

    // No credentials, or they no longer work: become an access point and let
    // the user fix it from a phone. This is the only path a person who did not
    // build the firmware can take.
    if (!s_set.ssid[0] || wifi_start() != ESP_OK) {
        char ap[32];
        snprintf(ap, sizeof ap, "AirWriter-%s", suffix);
        s_ap_mode = 1;
        strncpy(s_ip, "192.168.4.1", sizeof s_ip - 1);

        if (!s_set.ssid[0]) ESP_LOGW(TAG, "no wi-fi credentials stored");
        else                ESP_LOGW(TAG, "could not join \"%s\"", s_set.ssid);

        ESP_ERROR_CHECK(net_provision_start(ap));
        webui_cfg_t wcfg = { .status_json = status_json, .captive = 1 };
        ESP_ERROR_CHECK(webui_start(&wcfg));

        for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
    }

    ipp_server_cfg_t cfg = {
        .queue_name = s_set.queue,
        .hostname   = s_set.hostname,
        .port       = 631,
        .dev_mode   = AW_DEV_MODE,
        .sink = { .begin = doc_begin, .data = doc_data, .end = doc_end,
                  .cancel = doc_cancel, .ctx = NULL },
    };
    ESP_ERROR_CHECK(ipp_server_start(&cfg));

    webui_cfg_t wcfg = { .status_json = status_json, .captive = 0 };
    ESP_ERROR_CHECK(webui_start(&wcfg));

    ESP_LOGI(TAG, "ready: %s", ipp_server_uri());
    ESP_LOGI(TAG, "settings at http://%s.local/", s_set.hostname);
    ESP_LOGI(TAG, "%u bytes free after startup", (unsigned)esp_get_free_heap_size());

    for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
}
