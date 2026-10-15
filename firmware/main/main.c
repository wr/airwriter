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
#include "led_grid_core.h"
#include "printer_state.h"
#include "serial_out.h"
#include "settings.h"
#include "webui.h"
#include "net_provision.h"
#include "pic_prog.h"
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

// Reconnect after a drop. Once the board has had the network it never falls
// back to setup mode on its own -- a router reboot is no reason to make the
// user re-provision -- it keeps trying, backing off 1, 2, 4 ... 30 s.
static int s_wifi_was_up;
static uint32_t s_backoff_ms = 1000;
static esp_timer_handle_t s_reconnect;

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

// Last job, for the status page.
static char   s_last_job[64];
static int    s_printing;
static size_t s_last_pages, s_last_bytes;

// The real mutual exclusion between a real IPP job and a test page: taken
// (blocking) at the top of doc_begin and (non-blocking) in test_page, given
// back in job_finished -- when the job has printed, not when its upload
// ended -- and at the bottom of test_page_task. s_printing above is
// merely the status page's idea of "busy" -- it is not itself a lock, since
// checking it and then setting it is not atomic across tasks.
//
// A binary semaphore, not a mutex: test_page takes it on dip_config's poll
// task and test_page_task gives it back, and a mutex given by a task that
// does not hold it trips FreeRTOS's priority-disinherit assert (W-1033).
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
// job_finished() reads it for the post-mortem.
static int s_printer_failed;

// Set by doc_cancel: the job ended because the person cancelled it, which the
// panel shows as done, not as a red failure.
static volatile int s_job_cancelled;

// Set by doc_begin to record whether job_pipe_begin actually started the
// pipeline task. doc_end reads it to decide whether job_pipe_end is safe to
// call.
static int s_job_pipe_ok;

// Whether the last job's stream was cut mid-command (W-1041). Only the task
// holding s_job_lock touches it: doc_begin through job_finished, or a test
// page.
static iw_link_t s_link;

static void resync_out(void *ctx, const uint8_t *buf, size_t n)
{
    esp_err_t *err = (esp_err_t *)ctx;
    if (*err == ESP_OK) *err = serial_out_write(buf, n);
}

// A write failed: the printer stayed paused past STALL_ABORT, or the job was
// cancelled. Abort the job so the rip task winds down instead of pushing the
// rest of the page at a dead line.
static void printer_fail(void)
{
    s_printer_failed = 1;
    // The dead job's queued tail must not reach the printer when paper
    // returns -- it would print garbage ahead of the next job.
    serial_out_purge();
    job_pipe_abort();
}

// doc_end's verdict on the transfer, for job_finished on the pipeline task.
static volatile int s_doc_ok;

// The printer stream: checksummed so the device can be compared with tools/rip
// byte for byte, then put on the wire.
static void printer_out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    s_stream_crc = crc32_up(s_stream_crc, buf, n);
    if (s_printer_failed) return;
    // The last job was cut short, so the printer may still be counting a
    // strip's graphics bytes: flush that before this job's first byte. Flow
    // controlled like the job, so it waits out a paused printer; outside the
    // CRC, which is the job's stream alone. Loopback has no printer to resync,
    // and the extra bytes would fail its byte count.
    if (iw_link_need_resync(&s_link) && !AW_LOOPBACK) {
        ESP_LOGW(TAG, "last job was cut short; resyncing the printer first");
        esp_err_t err = ESP_OK;
        iw_resync(resync_out, &err);
        if (err != ESP_OK) { printer_fail(); return; }
    }
    if (serial_out_write(buf, n) != ESP_OK) printer_fail();
}

// HSKo edges from serial_out. printer_state turns a low line into "printer
// offline" (idle) or "needs attention" (mid-job) on its own clock.
static void printer_line_changed(void *ctx, int ready)
{
    (void)ctx;
    printer_state_post(ready ? PS_EV_LINE_READY : PS_EV_LINE_NOT_READY);
}

static esp_err_t doc_begin(void *ctx, const char *format, const char *name)
{
    (void)ctx;
    // A real job waits out a test page rather than interleaving with it,
    // mirroring how ipp_server's own s_print_lock makes a second phone job
    // wait for the first. ipp_server always pairs this call with exactly one
    // doc_end -- including when job_pipe_begin below fails -- and every
    // doc_end leads to exactly one job_finished, whose give balances every
    // take here, refusals included.
    xSemaphoreTake(s_job_lock, portMAX_DELAY);
    // The link's strength with each job: a weak one is what a slow upload
    // usually is (W-1042 bench).
    wifi_ap_record_t ap;
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    ESP_LOGI(TAG, "job \"%s\" format %s, rssi %d", name,
             format[0] ? format : "(unspecified)", rssi);
    strncpy(s_last_job, name && name[0] ? name : "Untitled", sizeof s_last_job - 1);
    s_last_job[sizeof s_last_job - 1] = '\0';
    s_stream_crc = 0;
    s_printer_failed = 0;
    s_job_cancelled = 0;
    serial_out_set_abort(0);
    iw_link_job_start(&s_link);
    s_printing = 1;
    printer_state_post(PS_EV_JOB_START);
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
    s_job_cancelled = 1;
    serial_out_set_abort(1);    // a paused printer must not hold the job hostage
    job_pipe_abort();
}

// The job is over: ripped and written, or abandoned. Runs on the pipeline
// task once it has finished with the document (or on the IPP worker, if the
// pipeline never started), and is what ends the job everywhere -- the panel,
// the job lock, and the IPP job record with the printer it holds.
static void job_finished(const job_pipe_stats_t *st, int ok)
{
    s_printing = 0;

    // Clean only if the rip finished every page and every byte was queued;
    // anything else may have stopped mid-strip.
    int clean = st->rc == 0 && ok && !s_printer_failed;

    if (!clean) {
        iw_link_job_end(&s_link, 0);
        // The rip may have stopped between writes, leaving up to a FIFO of
        // the dead job queued behind a paused printer. This is the writing
        // task, or no write ever ran.
        serial_out_purge();
        if (s_job_cancelled) {
            ESP_LOGI(TAG, "job canceled after %u bytes in", (unsigned)st->doc_bytes);
            printer_state_post(PS_EV_JOB_DONE);
        } else {
            ESP_LOGE(TAG, "job failed: %u bytes in, rc %d%s",
                     (unsigned)st->doc_bytes, st->rc,
                     s_printer_failed ? ", printer timeout"
                                      : ok ? "" : ", transfer truncated");
            printer_state_post(PS_EV_JOB_FAILED);
        }
        xSemaphoreGive(s_job_lock);
        ipp_server_job_done(0);
        return;
    }

    s_last_pages = (size_t)st->pages;
    s_last_bytes = st->printer_bytes;
    ESP_LOGI(TAG, "ripped %d page%s, %ux%u dots, %u bytes in -> %u printer bytes in %ums",
             st->pages, st->pages == 1 ? "" : "s", (unsigned)st->dst_w, (unsigned)st->dst_h,
             (unsigned)st->doc_bytes, (unsigned)st->printer_bytes, (unsigned)st->ms);
    ESP_LOGI(TAG, "printer stream crc32 %08x", (unsigned)s_stream_crc);

    // Let the last byte clear the shift register before calling it printed.
    // Paper running out on the last sheet is still this job's stall, so the
    // flush waits as long as a paused job does, and Done comes after it. A
    // cancel stops the wait, and the tail must not print ahead of the next job.
    esp_err_t flushed = serial_out_flush(CONFIG_AIRWRITER_STALL_ABORT * 1000u);
    // A purged tail is a cut stream too, so the link is settled only here.
    iw_link_job_end(&s_link, flushed == ESP_OK);
    if (flushed != ESP_OK) {
        serial_out_purge();
        if (s_job_cancelled) {
            ESP_LOGI(TAG, "job canceled with its last bytes still queued");
            printer_state_post(PS_EV_JOB_DONE);
        } else {
            ESP_LOGE(TAG, "job failed: the printer never took its last bytes");
            printer_state_post(PS_EV_JOB_FAILED);
        }
        xSemaphoreGive(s_job_lock);
        ipp_server_job_done(0);
        return;
    }
    printer_state_post(PS_EV_JOB_DONE);

    if (AW_LOOPBACK) {
        size_t got; uint32_t crc;
        serial_out_loopback_wait(st->printer_bytes, 3000);
        serial_out_loopback_stats(&got, &crc);
        if (got == st->printer_bytes && crc == s_stream_crc)
            ESP_LOGI(TAG, "uart loopback: %u bytes returned intact, crc32 %08x",
                     (unsigned)got, (unsigned)crc);
        else
            ESP_LOGE(TAG, "uart loopback MISMATCH: sent %u/%08x, got %u/%08x",
                     (unsigned)st->printer_bytes, (unsigned)s_stream_crc,
                     (unsigned)got, (unsigned)crc);
    }

    // At 9600 baud, 8N1, one byte costs 10 bit times.
    ESP_LOGI(TAG, "that is %u seconds of printing at 9600 baud",
             (unsigned)(st->printer_bytes * 10 / 9600));

    xSemaphoreGive(s_job_lock);
    ipp_server_job_done(1);
}

// job_pipe's done callback: the pipeline has printed the document or let it go.
static void pipe_done(void *ctx, const job_pipe_stats_t *st)
{
    (void)ctx;
    job_finished(st, s_doc_ok);
}

// The upload is over; the printing usually is not. Returns at once so the
// client gets its answer while the pipeline prints from the job buffer, and
// pipe_done finishes the job when the paper stops.
static void doc_end(void *ctx, int ok)
{
    (void)ctx;
    s_doc_ok = ok;
    // A cut transfer (a client that cancels by dropping the upload rather
    // than with Cancel-Job) means nothing more will arrive: stop a write
    // that is waiting out a paused printer, so the pipeline unwinds now
    // instead of after the stall limit.
    if (!ok) serial_out_set_abort(1);
    if (s_job_pipe_ok) {
        job_pipe_end(ok);
        return;
    }
    // A failed job_pipe_begin never created the pipeline task, so nothing
    // will call pipe_done: finish here, with zeroed stats. ipp_server still
    // calls doc_end exactly once after a failed begin, so the give in
    // job_finished balances doc_begin's take.
    job_pipe_stats_t st = {0};
    job_finished(&st, 0);
}

// ------------------------------------------------------------- dip / test
// DIP1-8 select mode/dev-mode/test-on-boot/dither-override/spare; a press of
// the test button prints a self-test page. dev_mode and dither_ovr land in
// later milestones -- for this rev dev_mode is a log note only, and the
// printer-uuid behaviour it names stays Kconfig's AIRWRITER_DEV_MODE.

// W-1039: unstick a printer that serial-line noise (a cable replug, power-up)
// left busy with SELECT lit -- CTRL-X cancels the unprinted line in its
// buffer. printer_state decides when (idle, busy 5 s, then every 60 s at
// most); the job lock, taken without waiting, keeps it clear of any job's
// writes or a test page.
static void kick_task(void *arg)
{
    (void)arg;
    static const uint8_t ctrl_x = 0x18;     // ImageWriter II: cancel current line
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!printer_state_want_kick()) continue;
        if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) continue;
        esp_err_t err = serial_out_send_unflowed(&ctrl_x, 1);
        printer_state_kicked();
        xSemaphoreGive(s_job_lock);
        ESP_LOGI(TAG, "printer busy while idle; sent CTRL-X (%s)", esp_err_to_name(err));
    }
}

// A person did something; wake the panel. From the IPP, DIP and test-button paths.
static void on_activity(void)
{
    printer_state_post(PS_EV_ACTIVITY);
}

#define TESTPAGE_W 1152
#define TESTPAGE_H 64

static void test_page_task(void *arg)
{
    (void)arg;
    s_stream_crc = 0;
    s_printer_failed = 0;
    s_job_cancelled = 0;
    serial_out_set_abort(0);
    iw_link_job_start(&s_link);
    printer_state_post(PS_EV_JOB_START);

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
    iw_link_job_end(&s_link, !s_printer_failed);

    serial_out_flush(CONFIG_AIRWRITER_STALL_ABORT * 1000u);
    printer_state_post(s_printer_failed ? PS_EV_JOB_FAILED : PS_EV_JOB_DONE);
    s_printing = 0;
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

// Runs on whatever task calls it -- normally dip_config's poll task, for a
// button press, which is why this only ever spawns the real work rather than
// doing it here: the poll loop has to stay free to keep debouncing.
static void test_page(void)
{
    on_activity();
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
    on_activity();
    ESP_LOGI(TAG, "dip: mode=%u dev_mode=%u test_on_boot=%u dither_ovr=%u gamer=%u spare=%u",
             cfg->mode, cfg->dev_mode, cfg->test_on_boot, cfg->dither_ovr, cfg->gamer,
             cfg->spare);
    led_grid_set_gamer(cfg->gamer);
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
        if (s_wifi_was_up) {
            printer_state_post(PS_EV_WIFI_DOWN);
            strncpy(s_ip, "0.0.0.0", sizeof s_ip - 1);
            ESP_LOGW(TAG, "wifi lost (reason %d); retrying in %us",
                     d ? d->reason : -1, (unsigned)(s_backoff_ms / 1000));
            esp_timer_start_once(s_reconnect, (uint64_t)s_backoff_ms * 1000);
            s_backoff_ms = s_backoff_ms >= 15000 ? 30000 : s_backoff_ms * 2;
        } else if (s_retries < WIFI_MAX_RETRY) {
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
        s_wifi_was_up = 1;
        s_backoff_ms  = 1000;
        printer_state_post(PS_EV_WIFI_UP);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_netif_t *s_sta_netif;

// Everything both the join and the setup-portal paths stand on. Runs whether or
// not credentials exist: a fresh board has none, skips the join entirely, and
// net_provision_start() aborts in esp_netif_create_default_wifi_ap() if the
// event loop and driver were never brought up (W-1032).
static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, s_set.hostname);

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    const esp_timer_create_args_t ta = { .callback = reconnect_cb, .name = "wifi_rejoin" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_reconnect));
}

// Setup mode: count phones on the access point, for the solid-blue cue.
static void ap_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    static unsigned n;
    if (id == WIFI_EVENT_AP_STACONNECTED) n++;
    else if (id == WIFI_EVENT_AP_STADISCONNECTED && n) n--;
    printer_state_set_ap_stations(n);
}

// The handlers are registered here, not in wifi_init(), because wifi_event()
// calls esp_wifi_connect() on STA_START -- in setup mode that would send the
// station chasing an empty SSID and drag the access point's channel with it.
static esp_err_t wifi_start(void)
{
    esp_netif_t *netif = s_sta_netif;
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
    // The default fast scan joins the first access point that answers, and
    // channels are scanned in order. With a mesh, that is often a far node on
    // channel 1: rev A on the bench joined one at -80 to -87 dBm with a node at
    // -46 on channel 11 in the same room, and uploads crawled at 6 KB/s.
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
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

static const char *state_text(ps_state_t st)
{
    switch (st) {
    case PS_BOOTING:   return "Starting";
    case PS_SETUP:     return "Waiting for setup";
    case PS_JOINING:   return "Joining Wi-Fi";
    case PS_ATTENTION: return "Needs attention";
    case PS_FAILED:    return "Last job failed";
    case PS_PRINTING:  return "Printing";
    case PS_OFFLINE:   return "Printer offline";
    case PS_READY:     return "Ready";
    }
    return "?";
}

// Why the chip last started. A reboot on the bench is otherwise a mystery: the
// USB console re-enumerates with the chip and loses the ROM's rst: line, so
// power-on (a pulled cable) and brownout or panic look the same from outside.
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "reset pin";
    case ESP_RST_SW:        return "software";      // esp_restart: OTA, settings
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";           // a console attach resets
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}

static void status_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    const esp_app_desc_t *app = esp_app_get_description();
    ps_view_t v = printer_state_view();
    int attention = v.state == PS_ATTENTION, offline = v.state == PS_OFFLINE;
    int failed    = v.state == PS_FAILED;
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
        "{\"state\":\"%s\",\"ps\":\"%s\",\"asleep\":%s,\"stopped\":%s,\"setup\":%s,\"job\":\"%s\","
        "\"fault\":%s,\"printer\":%s,\"printing\":%s,"
        "\"queue\":\"%s\",\"host\":\"%s.local\",\"ip\":\"%s\","
        "\"ssid\":\"%s\",\"dither\":\"%s\",\"signal\":\"%s\","
        "\"channel\":\"%s\",\"security\":\"%s\",\"uri\":\"%s\","
        "\"uptime\":\"%uh %02um\",\"reset\":\"%s\",\"version\":\"%s\",\"heap\":\"%u KB\","
        "\"model\":\"Apple ImageWriter II\",\"pic\":%s}",
        state_text(v.state),
        ps_name(v.state),
        v.asleep ? "true" : "false",
        attention || offline ? "true" : "false",
        s_ap_mode ? "true" : "false",
        job,
        attention || failed ? "true" : "false",
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
        reset_name(esp_reset_reason()), app->version, (unsigned)(esp_get_free_heap_size() / 1024),
        pic_prog_available() ? "true" : "false");
}

static void localtalk_flash(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    pic_result_t r;
    pic_prog_flash_tashtalk(&r);
    pic_result_json(&r, buf, cap);
}

// ----------------------------------------------------------------- main

// Runs on printer_state's poll task, once per change of display state.
static void on_ps_change(const ps_view_t *v)
{
    ESP_LOGI(TAG, "state -> %s", ps_name(v->state));
    // SPEC.md 3.9: a stall mid-job becomes a real alert on the phone.
    // Mid-job stall: media-empty, the real "out of paper" alert. The -error
    // suffix (RFC 8011 5.4.12) says it outright: a stopped printer MUST carry
    // an error reason, and a bare one is only "assumed" to be one.
    // Idle and not ready: printer off, unplugged or deselected. offline-report
    // is CUPS-only (not in the IANA registry), so it reports the registered
    // "other", which on a stopped printer reads as an error all the same.
    if (v->state == PS_ATTENTION)
        ipp_server_set_stopped("media-empty-error", "Needs attention: check paper and SELECT");
    else if (v->state == PS_OFFLINE)
        ipp_server_set_stopped("other", "Printer offline");
    else
        ipp_server_set_stopped(NULL, NULL);
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
    // IO4 runs through R15 to the PIC's RA1, TashTalk's UART receive. Left
    // floating, TashTalk can decode noise as host commands and put garbage
    // on a LocalTalk bus. High is the UART idle level; low would be a break.
    // pic_prog borrows the pin for a flash and hands it back as a pulled-up
    // input; a LocalTalk UART driver will own it once that lands. Off the S3
    // the default is -1 (IO4 is the C6 devkit's printer TX); a devkit wired
    // to a breadboard PIC sets its own pin and gets the same clamp.
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
    gpio_config_t lt_tx_clamp = {
        .pin_bit_mask = 1ULL << CONFIG_AIRWRITER_PIC_CLK_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(CONFIG_AIRWRITER_PIC_CLK_PIN, 1);
    gpio_config(&lt_tx_clamp);
#endif
    // IO35 is the SP3232's T1IN, a driver input with no pull-up that must be
    // tied. T1OUT reaches only the DNP R9 on rev A, but a floating CMOS input
    // can idle at threshold, on the same chip that carries the CTS receiver.
    // Rev B ties T1IN to GND.
#if CONFIG_AIRWRITER_HSKI_DRV_PIN >= 0
    gpio_config_t hski_clamp = {
        .pin_bit_mask = 1ULL << CONFIG_AIRWRITER_HSKI_DRV_PIN,
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(CONFIG_AIRWRITER_HSKI_DRV_PIN, 0);
    gpio_config(&hski_clamp);
#endif

    // Before anything that can call doc_begin or test_page: dip_config's poll
    // task starts soon and a button press must not race the lock into
    // existence.
    s_job_lock = xSemaphoreCreateBinary();
    xSemaphoreGive(s_job_lock);             // created empty; start it free

    ps_timing_t tm = PS_TIMING_DEFAULT;
    tm.stall_attention_ms = CONFIG_AIRWRITER_STALL_TIMEOUT * 1000u;
    tm.boot_ms = LG_BOOT_MS;
    printer_state_start(&tm, on_ps_change);
    // After the clamp above: the RMT driver takes IO48 over from here, and
    // by the first frame the line is driven, never floating.
    ESP_ERROR_CHECK(led_grid_start());

    ESP_LOGI(TAG, "AirWriter starting after %s reset, %u bytes free",
             reset_name(esp_reset_reason()), (unsigned)esp_get_free_heap_size());

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
        .stall_abort_s   = CONFIG_AIRWRITER_STALL_ABORT,
        .on_line         = printer_line_changed,
        .loopback        = AW_LOOPBACK,
    };
    ESP_ERROR_CHECK(serial_out_init(&ser));
    xTaskCreate(kick_task, "kick", 3072, NULL, 3, NULL);

    job_pipe_cfg_t pipe = { .out = printer_out, .dither = algo, .done = pipe_done };
    ESP_ERROR_CHECK(job_pipe_init(&pipe));

    // Before wifi_start, which can block up to 60s joining: the debounce
    // settles in ~300ms, so the boot-time read is long ready by the time
    // test_on_boot is checked below, whichever path gets there.
    dip_config_cb_t dip_cb = { .on_change = dip_on_change, .on_test_button = test_page };
    ESP_ERROR_CHECK(dip_config_start(&dip_cb));

    // No credentials, or they no longer work: become an access point and let
    // the user fix it from a phone. This is the only path a person who did not
    // build the firmware can take.
    printer_state_post(PS_EV_WIFI_JOINING);
    wifi_init();
    if (!s_set.ssid[0] || wifi_start() != ESP_OK) {
        char ap[32];
        snprintf(ap, sizeof ap, "AirWriter-%s", suffix);
        s_ap_mode = 1;
        strncpy(s_ip, "192.168.4.1", sizeof s_ip - 1);
        printer_state_post(PS_EV_WIFI_SETUP_AP);

        if (!s_set.ssid[0]) ESP_LOGW(TAG, "no wi-fi credentials stored");
        else                ESP_LOGW(TAG, "could not join \"%s\"", s_set.ssid);

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_AP_STACONNECTED, ap_event, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, WIFI_EVENT_AP_STADISCONNECTED, ap_event, NULL, NULL));
        ESP_ERROR_CHECK(net_provision_start(ap));
        webui_cfg_t wcfg = { .status_json = status_json, .captive = 1,
                              .localtalk_flash = localtalk_flash };
        ESP_ERROR_CHECK(webui_start(&wcfg));

        for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
    }

    ipp_server_cfg_t cfg = {
        .queue_name = s_set.queue,
        .hostname   = s_set.hostname,
        .port       = 631,
        .dev_mode   = AW_DEV_MODE,
        .on_job_op  = on_activity,
        .sink = { .begin = doc_begin, .data = doc_data, .end = doc_end,
                  .cancel = doc_cancel, .ctx = NULL },
    };
    ESP_ERROR_CHECK(ipp_server_start(&cfg));

    webui_cfg_t wcfg = { .status_json = status_json, .captive = 0,
                          .localtalk_flash = localtalk_flash };
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
