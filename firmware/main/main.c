// SPDX-License-Identifier: GPL-3.0-or-later
//
// AirWriter application entry point.
//
// M4 scope: join WiFi, advertise over mDNS, answer IPP. A job is accepted and
// counted, not printed -- serial_out and printer_state land with the board.
#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
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
#include "freertos/semphr.h"

#include "dip_config.h"
#include "halftone.h"
#include "imagewriter.h"
#include "ipp_server.h"
#include "job_pipe.h"
#include "led_grid.h"
#include "printer_state.h"
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
#ifdef CONFIG_AIRWRITER_INVERT_CTS
#define AW_INVERT_CTS 1
#else
#define AW_INVERT_CTS 0
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
static int    s_printing;
static size_t s_last_pages, s_last_bytes;

// The real mutual exclusion between a real IPP job and a test page: taken
// (blocking) at the top of doc_begin and (non-blocking) in test_page, given
// back at the bottom of doc_end and test_page_task. s_printing above is
// merely the status page's idea of "busy" -- it is not itself a lock, since
// checking it and then setting it is not atomic across tasks.
static SemaphoreHandle_t s_job_lock;

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

// Set when serial_out gave up on a stalled printer mid-job. job_pipe_cfg_t's
// out callback returns void, so the failure travels in this flag instead and
// doc_end() reads it for the post-mortem.
static int s_printer_failed;

// Set by doc_begin to record whether job_pipe_begin actually started the
// pipeline task. doc_end reads it to decide whether job_pipe_end is safe to
// call.
static int s_job_pipe_ok;

// The printer stream: checksummed so the device can be compared with tools/rip
// byte for byte, then put on the wire. A write failure means the watchdog
// called the printer gone; abort the job so the rip task winds down instead
// of pushing the rest of the page at a dead line.
static void printer_out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    s_stream_crc = crc32_up(s_stream_crc, buf, n);
    if (s_printer_failed) return;
    if (serial_out_write(buf, n) != ESP_OK) {
        s_printer_failed = 1;
        // The dead job's queued tail must not reach the printer when paper
        // returns -- it would print garbage ahead of the next job.
        serial_out_purge();
        job_pipe_abort();
    }
}

// HSKo has been low too long. SPEC.md 3.9: that becomes a real alert on the
// phone rather than a job that quietly never finishes.
static void printer_state_changed(void *ctx, serial_out_state_t st)
{
    (void)ctx;
    ipp_server_set_stopped(st == SERIAL_OUT_BLOCKED ? "media-empty" : NULL);
    printer_state_post_ev(st == SERIAL_OUT_BLOCKED ? PS_EV_PRINTER_BLOCKED
                                                    : PS_EV_PRINTER_CLEARED);
}

static esp_err_t doc_begin(void *ctx, const char *format, const char *name)
{
    (void)ctx;
    // A real job waits out a test page rather than interleaving with it,
    // mirroring how ipp_server's own s_print_lock makes a second phone job
    // wait for the first. ipp_server always pairs this call with exactly one
    // doc_end -- including when job_pipe_begin below fails -- so the give in
    // doc_end balances every take here, refusals included.
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "job \"%s\" format %s", name, format[0] ? format : "(unspecified)");
    strncpy(s_last_job, name && name[0] ? name : "Untitled", sizeof s_last_job - 1);
    s_last_job[sizeof s_last_job - 1] = '\0';
    s_stream_crc = 0;
    s_printer_failed = 0;
    s_printing = 1;
    printer_state_post_ev(PS_EV_JOB_START);
    serial_out_loopback_reset();
    esp_err_t err = job_pipe_begin(name);
    s_job_pipe_ok = (err == ESP_OK);
    return err;
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
    job_pipe_stats_t st = {0};
    // A failed job_pipe_begin never created the pipeline task, so job_pipe_end
    // would block on a semaphore nothing can ever give -- up to its 600s
    // stuck-pipeline timeout -- while holding s_job_lock the whole time. Skip
    // the call and report zeroed stats instead. ipp_server still calls
    // doc_end exactly once even after a failed begin, so the give below still
    // balances doc_begin's take.
    if (s_job_pipe_ok) job_pipe_end(ok, &st);
    s_printing = 0;

    if (st.rc != 0 || !ok || s_printer_failed) {
        ESP_LOGE(TAG, "job failed: %u bytes in, rc %d%s",
                 (unsigned)st.doc_bytes, st.rc,
                 s_printer_failed ? ", printer timeout"
                                  : ok ? "" : ", transfer truncated");
        printer_state_post_ev(PS_EV_JOB_FAIL);
        xSemaphoreGive(s_job_lock);
        return;
    }

    printer_state_post_ev(PS_EV_JOB_DONE);
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

    xSemaphoreGive(s_job_lock);
}

// ------------------------------------------------------------- dip / test
// DIP1-8 select mode/dev-mode/test-on-boot/dither-override/spare; a press of
// the test button prints a self-test page. dev_mode and dither_ovr land in
// later milestones -- for this rev dev_mode is a log note only, and the
// printer-uuid behaviour it names stays Kconfig's AIRWRITER_DEV_MODE.

#define TESTPAGE_W 1152
#define TESTPAGE_H 64

static void test_page_task(void *arg)
{
    (void)arg;
    s_stream_crc = 0;
    s_printer_failed = 0;
    printer_state_post_ev(PS_EV_JOB_START);

    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;

    ht_t h;
    if (ht_init(&h, algo, TESTPAGE_W) != 0) {
        ESP_LOGE(TAG, "test page: ht_init failed");
        s_printer_failed = 1;
    } else {
        static iw_t w;
        iw_init(&w, TESTPAGE_W, printer_out, NULL);
        iw_begin_page(&w);

        uint8_t gray[TESTPAGE_W];
        uint8_t bits[(TESTPAGE_W + 7) / 8];
        for (uint32_t y = 0; y < TESTPAGE_H; y++) {
            if (y < TESTPAGE_H - 8) {
                // 8px checkerboard bands, alternating phase band to band, so a
                // dot-alignment error shows as a shifted grid rather than a
                // gray smear.
                int phase = (int)(y / 8) & 1;
                for (uint32_t x = 0; x < TESTPAGE_W; x++)
                    gray[x] = (((x / 8) & 1) == (uint32_t)phase) ? 0 : 255;
            } else {
                // Final band: a 0->255 gradient, one dithered level per column
                // group, to check the algorithm across the whole gray range.
                for (uint32_t x = 0; x < TESTPAGE_W; x++)
                    gray[x] = (uint8_t)(x * 255 / (TESTPAGE_W - 1));
            }
            ht_row(&h, gray, bits);
            iw_row(&w, bits);
        }

        iw_end_page(&w);
        ht_free(&h);
    }

    serial_out_flush(120000);
    printer_state_post_ev(s_printer_failed ? PS_EV_JOB_FAIL : PS_EV_JOB_DONE);
    s_printing = 0;
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

// Runs on whatever task calls it -- normally dip_config's poll task, for a
// button press, which is why this only ever spawns the real work rather than
// doing it here: the poll loop has to stay free to keep debouncing.
static void test_page(void)
{
    // Non-blocking: a test page refuses rather than queuing behind whatever
    // is printing, real job or another test page. s_job_lock is the actual
    // mutual exclusion; the take-then-spawn sequencing below (not s_printing)
    // is what makes two back-to-back presses refuse rather than both print.
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "test page requested but the printer is already busy");
        return;
    }
    s_printing = 1;
    if (xTaskCreate(test_page_task, "test_page", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "test page: could not create task");
        s_printing = 0;
        xSemaphoreGive(s_job_lock);
    }
}

// Latched by dip_on_change from the boot-time read, consumed once app_main
// has brought the servers up.
static int s_test_on_boot;

static void dip_on_change(const dip_config_t *cfg)
{
    ESP_LOGI(TAG, "dip: mode=%u dev_mode=%u test_on_boot=%u dither_ovr=%u spare=%u",
             cfg->mode, cfg->dev_mode, cfg->test_on_boot, cfg->dither_ovr, cfg->spare);
    if (cfg->dev_mode)
        ESP_LOGI(TAG, "dip: dev_mode is on, but the printer uuid stays Kconfig-driven this rev");
    if (cfg->test_on_boot) s_test_on_boot = 1;
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
        printer_state_post_ev(PS_EV_WIFI_UP);
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

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "Open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "Secured";
    }
}

static void status_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    const esp_app_desc_t *app = esp_app_get_description();
    int blocked = serial_out_state() == SERIAL_OUT_BLOCKED;
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);

    // Details of the network actually joined, for the Wi-Fi panel. Absent in
    // setup mode, where there is no association to describe.
    wifi_ap_record_t ap;
    int joined = !s_ap_mode && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;

    char job[96];
    if (s_last_job[0])
        snprintf(job, sizeof job, "%s (%u page%s)", s_last_job,
                 (unsigned)s_last_pages, s_last_pages == 1 ? "" : "s");
    else
        snprintf(job, sizeof job, "none yet");

    snprintf(buf, cap,
        "{\"state\":\"%s\",\"ps\":\"%s\",\"stopped\":%s,\"setup\":%s,\"job\":\"%s\","
        "\"fault\":%s,\"printer\":%s,\"printing\":%s,"
        "\"queue\":\"%s\",\"host\":\"%s.local\",\"ip\":\"%s\","
        "\"ssid\":\"%s\",\"dither\":\"%s\",\"signal\":\"%s\","
        "\"channel\":\"%s\",\"security\":\"%s\",\"uri\":\"%s\","
        "\"uptime\":\"%uh %02um\",\"version\":\"%s\",\"heap\":\"%u KB\","
        "\"model\":\"Apple ImageWriter II\"}",
        s_ap_mode ? "Waiting for setup" : blocked ? "Out of paper"
                : s_printing ? "Printing" : "Ready",
        ps_name(printer_state_get()),
        blocked ? "true" : "false",
        s_ap_mode ? "true" : "false",
        job,
        blocked ? "true" : "false",
        serial_out_clear() ? "true" : "false",
        s_printing ? "true" : "false", s_set.queue, s_set.hostname, s_ip,
        joined ? (char *)ap.ssid : (s_ap_mode ? "" : s_set.ssid),
        s_set.dither,
        joined ? (ap.rssi > -55 ? "Excellent" : ap.rssi > -67 ? "Good"
                : ap.rssi > -75 ? "Fair" : "Weak") : "",
        joined ? "2.4 GHz" : "",
        joined ? auth_name(ap.authmode) : "",
        s_ap_mode ? "" : ipp_server_uri(),
        (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
        app->version, (unsigned)(esp_get_free_heap_size() / 1024));
}

// ----------------------------------------------------------------- main

// Fires outside printer_state's lock, only on an actual state change.
static void on_ps_change(ps_state_t st)
{
    ESP_LOGI(TAG, "state -> %s", ps_name(st));
}

void app_main(void)
{
    // Before anything else: U9's level-shifter input has no pull, so from
    // power-on until this line the LED chain sees noise (a brief flicker at
    // boot is expected on rev A; rev B adds a 100k pulldown on U9.2). It must
    // run before wifi_start() especially -- the join loop can block for 60s.
    // Compile-time guard: the pin is -1 off the S3, and the C6 devkit's
    // headers have no GPIO 48.
#if CONFIG_AIRWRITER_LED_DATA_PIN >= 0
    gpio_config_t led_clamp = {
        .pin_bit_mask = 1ULL << CONFIG_AIRWRITER_LED_DATA_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(CONFIG_AIRWRITER_LED_DATA_PIN, 0);
    gpio_config(&led_clamp);
#endif

    // Before anything that can call doc_begin or test_page: dip_config's poll
    // task starts soon and a button press must not race the lock into
    // existence.
    s_job_lock = xSemaphoreCreateMutex();

    printer_state_start(on_ps_change);
    // After the clamp above: the RMT driver takes IO48 over from here, and
    // by the first frame the line is driven, never floating.
    ESP_ERROR_CHECK(led_grid_start());

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
        .invert_cts      = AW_INVERT_CTS,
        .stall_timeout_s = CONFIG_AIRWRITER_STALL_TIMEOUT,
        .on_state        = printer_state_changed,
        .loopback        = AW_LOOPBACK,
    };
    ESP_ERROR_CHECK(serial_out_init(&ser));

    job_pipe_cfg_t pipe = { .out = printer_out, .dither = algo };
    ESP_ERROR_CHECK(job_pipe_init(&pipe));

    // Before wifi_start, which can block up to 60s joining: the debounce
    // settles in ~300ms, so the boot-time read is long ready by the time
    // test_on_boot is checked below, whichever path gets there.
    dip_config_cb_t dip_cb = { .on_change = dip_on_change, .on_test_button = test_page };
    ESP_ERROR_CHECK(dip_config_start(&dip_cb));

    // No credentials, or they no longer work: become an access point and let
    // the user fix it from a phone. This is the only path a person who did not
    // build the firmware can take.
    printer_state_post_ev(PS_EV_WIFI_CONNECTING);
    if (!s_set.ssid[0] || wifi_start() != ESP_OK) {
        char ap[32];
        snprintf(ap, sizeof ap, "AirWriter-%s", suffix);
        s_ap_mode = 1;
        strncpy(s_ip, "192.168.4.1", sizeof s_ip - 1);
        printer_state_post_ev(PS_EV_SETUP_MODE);

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

    if (s_test_on_boot) {
        s_test_on_boot = 0;
        ESP_LOGI(TAG, "DIP4 was on at boot; printing a test page");
        test_page();
    }

    ESP_LOGI(TAG, "ready: %s", ipp_server_uri());
    ESP_LOGI(TAG, "settings at http://%s.local/", s_set.hostname);
    ESP_LOGI(TAG, "%u bytes free after startup", (unsigned)esp_get_free_heap_size());

    for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
}
