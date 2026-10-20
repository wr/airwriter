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
#include "localtalk.h"
#include "ethertalk.h"
#include "lt_node.h"
#include "ota_health.h"
#include "fw_update.h"
#include "cable_test.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#if CONFIG_BOOTLOADER_RESERVE_RTC_MEM
#include "bootloader_common.h"
#endif

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

// W-1046: the way back lives below the app, in the bootloader and the
// partition table, so the sdkconfig has to carry it. sdkconfig.defaults only
// seed a new sdkconfig; an older one keeps its old values, and this is where
// that shows rather than on a board that cannot recover.
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "sdkconfig predates W-1046: enable CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE (sdkconfig.defaults)"
#endif
// W-1059: the power-up reset is the bootloader hook's (airwriter_boot). IDF's
// own would erase otadata and boot ota_0 instead of the known-good slot.
#if CONFIG_BOOTLOADER_FACTORY_RESET
#error "sdkconfig predates W-1059: turn CONFIG_BOOTLOADER_FACTORY_RESET off (sdkconfig.defaults.esp32s3)"
#endif
#if !CONFIG_BOOTLOADER_RESERVE_RTC_MEM
#error "sdkconfig predates W-1059: set CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC (sdkconfig.defaults.esp32s3)"
#endif
#if CONFIG_AIRWRITER_TEST_BTN_PIN != 14
#error "the bootloader hook's reset pin (airwriter_boot, RESET_PIN) must be the test button"
#endif
// W-1048: an image that is not signed would refuse every update after it
// (IDF trusts the key that signed the running image), so every build is.
#if !CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME || \
    !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT || !CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES
#error "sdkconfig predates W-1048: copy the signing options from sdkconfig.defaults"
#endif
_Static_assert(LG_RESET_FILL_MS == BTN_FILL_MS,
               "the panel's countdown must run out when the button commits");

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

// W-1048: a firmware update holds s_job_lock from its start to the restart,
// and gives way to a job that arrives meanwhile. A job that finds the lock
// taken raises this; the update reads it between chunks and lets go. Only
// jobs raise it -- a refused CTRL-X kick or TashTalk flash is not a job.
static volatile int s_job_waiting;
// Uptime seconds of the last job's start or end, -1 for none yet:
// auto-update installs only once the board has been idle for a while.
static volatile int32_t s_last_job_s = -1;

static void job_touch(void)
{
    s_last_job_s = (int32_t)(esp_timer_get_time() / 1000000);
}

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

// An IPP job holds s_job_lock: from doc_begin's take to job_finished's give.
// A Cancel-Job for a job still waiting in doc_begin must not reach the printer
// another job owns -- a LocalTalk job holds it for minutes (W-1053 review).
// ipp_server passes the cancel on again once begin returns.
static volatile int s_ipp_owns;

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

// ------------------------------------------------------------- ribbon
// W-1064: which ribbon is in, from the printer's self-ID. What AirPrint
// clients see follows it (the colour attributes and TXT keys, same UUID), and
// RGB pages print in colour only while it says colour. Saved, so a restart
// advertises what the printer last said instead of black until it answers.
static volatile uint8_t    s_ribbon;        // iw_ribbon_t
static volatile int        s_ribbon_want;   // ask the idle printer, from s_ribbon_due on
static volatile TickType_t s_ribbon_due;
static volatile TickType_t s_line_low_at;   // when HSKo last went false
static volatile int        s_line_seen;     // serial_out has reported the line

static char s_suffix[8];            // the MAC's last two bytes, the default hostname's

// W-1079: what the page last saved, beside s_set's names in use since boot:
// a new name or hostname reaches mDNS and the Chooser only at a restart.
static char s_saved_model[SET_QUEUE_MAX], s_saved_name[SET_QUEUE_MAX], s_saved_host[SET_HOST_MAX];
// The DIP bank as last read, for the page (W-1079). Unknown until the first.
static dip_config_t s_dip;
static volatile int s_dip_known;

// The page saved queue, hostname, dither or gain (W-1068): re-read them, and
// give the pipe and the test page the screen they now name. The hostname
// and queue still take a restart to reach mDNS, as they always have.
static void lt_port_apply(int on);

static void general_saved(void *ctx)
{
    (void)ctx;
    settings_t fresh;
    if (settings_load(&fresh, s_suffix) != ESP_OK) return;
    strncpy(s_set.dither, fresh.dither, sizeof s_set.dither - 1);
    s_set.gain = fresh.gain;
    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;
    job_pipe_set_screen(algo, s_set.gain);
    ESP_LOGI(TAG, "screen now %s, gain %u%%", s_set.dither, s_set.gain);
    // W-1079: the names wait for a restart; the switches act now.
    snprintf(s_saved_model, sizeof s_saved_model, "%s", fresh.model);
    snprintf(s_saved_name, sizeof s_saved_name, "%s", fresh.name);
    snprintf(s_saved_host, sizeof s_saved_host, "%s", fresh.hostname);
    if (fresh.airprint != s_set.airprint) {
        s_set.airprint = fresh.airprint;
        ipp_server_set_airprint(s_set.airprint);
    }
    if (fresh.lt_port >= 0 && fresh.lt_port != s_set.lt_port) {
        s_set.lt_port = fresh.lt_port;
        lt_port_apply(s_set.lt_port);
    }
}

static const char *ribbon_name(uint8_t r)
{
    return r == IW_RIBBON_COLOUR ? "colour" : r == IW_RIBBON_BLACK ? "black" : "unknown";
}

// Asks the printer which ribbon is in. The caller holds s_job_lock, with
// nothing of a job sent yet. ESC ? needs the printer ready (a deselected one
// does not answer) and between commands (after a stream cut mid-graphics it
// would be taken as dots). Idle, a printer that is not ready, or one left
// mid-graphics, is left alone. For a job it must not be: a colour page on a
// ribbon swapped while the printer was off would print four passes in black.
// So a job waits for the line as its first write would, and sends the resync
// its first write would have sent.
static void ribbon_check(int for_job)
{
    if (AW_LOOPBACK) return;
    for (uint32_t ms = 0; for_job && !serial_out_clear() && !s_job_cancelled &&
                          ms < CONFIG_AIRWRITER_STALL_ABORT * 1000u; ms += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!serial_out_clear()) return;
    if (s_link.dirty) {
        if (!for_job) return;
        esp_err_t err = ESP_OK;
        iw_resync(resync_out, &err);
        if (err != ESP_OK) return;
        iw_link_resynced(&s_link);
        ESP_LOGW(TAG, "last job was cut short; resynced the printer before asking its ribbon");
    }
    static const uint8_t self_id[] = { 0x1B, '?' }, cr[] = { '\r' };
    uint8_t reply[16];
    serial_out_rx_drop();
    int n = serial_out_query(self_id, sizeof self_id, reply, sizeof reply, 500);
    // The printer acts on a command when it reaches the line holding it (the
    // note under Table 6-7), so if it said nothing, end the line. Idle, the
    // head is at the left margin and a CR moves nothing.
    if (n == 0) n = serial_out_query(cr, sizeof cr, reply, sizeof reply, 1000);
    if (n < 0) return;
    char hex[3 * sizeof reply + 1] = "";
    for (int i = 0; i < n; i++) snprintf(hex + 3 * i, 4, " %02x", reply[i]);
    iw_ribbon_t r = iw_self_id_ribbon(reply, (size_t)n);
    ESP_LOGI(TAG, "self-ID: %d byte%s%s, %s ribbon", n, n == 1 ? "" : "s", hex, ribbon_name(r));
    if (r == IW_RIBBON_UNKNOWN || r == s_ribbon) return;
    ESP_LOGI(TAG, "ribbon now %s (was %s)", ribbon_name(r), ribbon_name(s_ribbon));
    s_ribbon = (uint8_t)r;
    settings_save_ribbon((uint8_t)r);
    ipp_server_set_colour(r == IW_RIBBON_COLOUR);
}

// job_pipe asks as each job's rip starts, on the pipeline task, while the
// job holds s_job_lock.
static int colour_ok(void *ctx)
{
    (void)ctx;
    ribbon_check(1);
    return s_ribbon == IW_RIBBON_COLOUR;
}

// HSKo edges from serial_out. printer_state turns a low line into "printer
// offline" (idle) or "needs attention" (mid-job) on its own clock. A printer
// that comes ready may have had its ribbon changed: ask it once it settles.
// Only after the line was down a second or more (power, SELECT, the lid) or
// at its first report: the printer's own busy blips are not a ribbon swap,
// and a check whose CR blipped the line must not schedule the next.
static void printer_line_changed(void *ctx, int ready)
{
    (void)ctx;
    TickType_t now = xTaskGetTickCount();
    if (!ready) {
        s_line_low_at = now;
    } else if (!s_line_seen || now - s_line_low_at >= pdMS_TO_TICKS(1000)) {
        s_ribbon_due  = now + pdMS_TO_TICKS(2000);
        s_ribbon_want = 1;
    }
    s_line_seen = 1;
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
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        s_job_waiting = 1;
        xSemaphoreTake(s_job_lock, portMAX_DELAY);
    }
    s_ipp_owns = 1;
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
    job_touch();
    led_grid_set_source(LG_SRC_AIRPRINT);
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
    if (!s_ipp_owns) return;
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
    job_touch();

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
            printer_state_post(PS_EV_JOB_CANCELLED);
        } else {
            ESP_LOGE(TAG, "job failed: %u bytes in, rc %d%s",
                     (unsigned)st->doc_bytes, st->rc,
                     s_printer_failed ? ", printer timeout"
                                      : ok ? "" : ", transfer truncated");
            printer_state_post(PS_EV_JOB_FAILED);
        }
        s_ipp_owns = 0;
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
            printer_state_post(PS_EV_JOB_CANCELLED);
        } else {
            ESP_LOGE(TAG, "job failed: the printer never took its last bytes");
            printer_state_post(PS_EV_JOB_FAILED);
        }
        s_ipp_owns = 0;
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

    ota_health_printed();       // W-1059: this firmware prints; under the job lock
    s_ipp_owns = 0;
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

// ---------------------------------------------------------- AppleTalk jobs
// W-1053: a PAP job is the Mac driver's own ImageWriter stream, so it goes to
// the printer as it is -- no rip -- under the same job lock as an IPP job or a
// test page. at_printer.c spools it for whichever link it came in on (the
// cable, or W-1076's EtherTalk links) and calls these from its print task
// (begin from the link's task). Whether a job that ended was cut mid-command
// is read off its bytes (iw_scan): a Mac that cancels just closes the
// connection, wherever its stream is, and W-1041's resync then runs before the
// next job.

static iw_scan_t    s_lt_scan;
static size_t       s_lt_bytes;

static const char *lt_job_name(at_link_t link)
{
    switch (link) {
    case AT_LINK_TUNNEL: return "Basilisk II job";
    case AT_LINK_ZONE:   return "Infinite Mac job";
    case AT_LINK_RADIO:  return "EtherTalk job";
    default:             return "LocalTalk job";
    }
}

static int lt_print_begin(void *ctx, at_link_t link)
{
    (void)ctx;
    // Never waits: the LocalTalk task must keep answering the Mac, which is
    // told "busy" and asks again.
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        s_job_waiting = 1;
        return 1;
    }
    strncpy(s_last_job, lt_job_name(link), sizeof s_last_job - 1);
    s_stream_crc = 0;
    s_printer_failed = 0;
    s_job_cancelled = 0;
    s_lt_bytes = 0;
    iw_scan_init(&s_lt_scan);
    serial_out_set_abort(0);
    iw_link_job_start(&s_link);
    s_printing = 1;
    job_touch();
    led_grid_set_source(LG_SRC_LOCALTALK);     // W-1049: the stripes, not AirPrint's ink
    printer_state_post(PS_EV_JOB_START);
    serial_out_loopback_reset();
    printer_state_post(PS_EV_ACTIVITY);
    ESP_LOGI(TAG, "%s starts", lt_job_name(link));
    return 0;
}

static int lt_print_write(void *ctx, const uint8_t *d, size_t n)
{
    (void)ctx;
    s_stream_crc = crc32_up(s_stream_crc, d, n);
    iw_scan(&s_lt_scan, d, n);
    s_lt_bytes += n;
    if (s_printer_failed) return 1;
    if (iw_link_need_resync(&s_link) && !AW_LOOPBACK) {
        ESP_LOGW(TAG, "last job was cut short; resyncing the printer first");
        esp_err_t err = ESP_OK;
        iw_resync(resync_out, &err);
        if (err != ESP_OK) goto fail;
    }
    if (serial_out_write(d, n) == ESP_OK) return 0;
fail:
    // Paused past STALL_ABORT: the Mac is told (CloseConn) and its queued
    // tail must not print ahead of the next job.
    s_printer_failed = 1;
    serial_out_purge();
    return 1;
}

static int lt_print_end(void *ctx, int normal)
{
    (void)ctx;
    int clean = normal && !s_printer_failed && iw_scan_between(&s_lt_scan);
    esp_err_t flushed = clean ? serial_out_flush(CONFIG_AIRWRITER_STALL_ABORT * 1000u) : ESP_FAIL;
    // The last bytes never left: paper out on the last line, past the limit.
    if (clean && flushed != ESP_OK) s_printer_failed = 1;
    if (flushed != ESP_OK) serial_out_purge();
    iw_link_job_end(&s_link, flushed == ESP_OK);
    s_last_bytes = s_lt_bytes;
    s_last_pages = 0;
    // In the link's log too (/api/lt, /api/et): the console is not always attached.
    char line[100];
    snprintf(line, sizeof line, "job %s: %u bytes, crc32 %08x",
             flushed == ESP_OK ? (s_lt_bytes ? "printed" : "empty") : s_printer_failed ? "failed"
             : normal ? "cut short" : "abandoned",
             (unsigned)s_lt_bytes, (unsigned)s_stream_crc);
    at_printer_note(line);
    if (AW_LOOPBACK && flushed == ESP_OK) {
        size_t got; uint32_t crc;
        serial_out_loopback_wait(s_lt_bytes, 3000);
        serial_out_loopback_stats(&got, &crc);
        snprintf(line, sizeof line, "uart loopback: %u/%u bytes back, crc32 %08x: %s",
                 (unsigned)got, (unsigned)s_lt_bytes, (unsigned)crc,
                 got == s_lt_bytes && crc == s_stream_crc ? "intact" : "MISMATCH");
        at_printer_note(line);
    }
    // A Mac that cancels is a done job, as Cancel-Job is; a printer that gave
    // up is a failed one.
    printer_state_post(s_printer_failed ? PS_EV_JOB_FAILED : PS_EV_JOB_DONE);
    s_printing = 0;
    job_touch();
    // W-1059, under the job lock; an empty job (a Mac that opened and closed,
    // or an idle relay peer) proves nothing about this firmware.
    if (flushed == ESP_OK && s_lt_bytes) ota_health_printed();
    xSemaphoreGive(s_job_lock);
    return flushed == ESP_OK;
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
        // W-1064: the ribbon, once the line has been ready 2 s with no job.
        if (s_ribbon_want && (int32_t)(xTaskGetTickCount() - s_ribbon_due) >= 0 &&
            xSemaphoreTake(s_job_lock, 0) == pdTRUE) {
            s_ribbon_want = 0;
            ribbon_check(0);
            xSemaphoreGive(s_job_lock);
        }
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

// The test page holds s_job_lock, and a second tap cancels it (W-1049). The
// tap and the page's end decide under s_tap_mux, so a tap landing as the page
// finishes can never set its cancel on the next job instead.
static portMUX_TYPE s_tap_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_test_running;      // the test page holds the lock
static int s_test_cancel;       // a second tap asked to stop it

static void test_page_task(void *arg)
{
    (void)arg;
    s_stream_crc = 0;
    s_printer_failed = 0;
    iw_link_job_start(&s_link);
    led_grid_set_source(LG_SRC_AIRPRINT);
    printer_state_post(PS_EV_JOB_START);

    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;

    ht_t h;
    if (ht_init(&h, algo, TESTPAGE_W) != 0) {
        ESP_LOGE(TAG, "test page: ht_init failed");
        s_printer_failed = 1;
    } else {
        ht_set_gain(&h, s_set.gain);
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
    // As job_finished does: the last byte has to clear before the page counts
    // as printed, a cancel stops that wait, and a tail that did not go out is
    // purged so it cannot print ahead of the next job, which then resyncs.
    esp_err_t flushed = s_printer_failed ? ESP_FAIL
                                         : serial_out_flush(CONFIG_AIRWRITER_STALL_ABORT * 1000u);
    if (flushed != ESP_OK) serial_out_purge();
    iw_link_job_end(&s_link, flushed == ESP_OK);

    portENTER_CRITICAL(&s_tap_mux);
    s_test_running = 0;
    int cancelled = s_test_cancel;
    portEXIT_CRITICAL(&s_tap_mux);
    // A cancel that came too late to cut anything is a page that printed.
    printer_state_post(flushed == ESP_OK ? PS_EV_JOB_DONE
                       : cancelled ? PS_EV_JOB_CANCELLED : PS_EV_JOB_FAILED);
    s_printing = 0;
    job_touch();
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

// Starts the test page; s_job_lock is held, and the page's task gives it
// back. Only ever spawns the real work: the caller is normally dip_config's
// poll task, which has to stay free to keep debouncing.
static void test_page_locked(void)
{
    s_printing = 1;
    job_touch();
    serial_out_set_abort(0);
    portENTER_CRITICAL(&s_tap_mux);
    s_test_running = 1;
    s_test_cancel = 0;
    portEXIT_CRITICAL(&s_tap_mux);
    if (xTaskCreate(test_page_task, "test_page", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "test page: could not create task");
        portENTER_CRITICAL(&s_tap_mux);
        s_test_running = 0;
        portEXIT_CRITICAL(&s_tap_mux);
        s_printing = 0;
        job_touch();
        xSemaphoreGive(s_job_lock);
    }
}

// DIP4 at boot. Non-blocking: a test page never queues behind whatever is
// printing; s_job_lock is the actual mutual exclusion.
static void test_page(void)
{
    on_activity();
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "test page requested but the printer is already busy");
        s_job_waiting = 1;
        return;
    }
    test_page_locked();
}

// W-1055: the cable test, from the button or the web page, under the job lock
// (it borrows the printer port's pins). The panel shows the answer, and with
// no TashTalk to run the test, says that instead of blaming the cable (W-1063).
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
static void cable_test_locked(cable_result_t *r, int both)
{
    cable_test_run(r, both);
    printer_state_cable(r->err == ESP_OK && !ct_verdict_tested(r->verdict) ? PS_CABLE_NO_CHIP
                        : r->err == ESP_OK && r->verdict == CT_PASS        ? PS_CABLE_PASS
                                                                           : PS_CABLE_FAIL);
}

static void cable_button_task(void *arg)
{
    (void)arg;
    cable_result_t r;
    cable_test_locked(&r, 0);
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}
#endif

// A tap of the test button. While the test page prints, a second tap cancels
// it (W-1049); during anyone else's job the panel says busy. Otherwise a cable
// from J2 to J4 leaves the printer's handshake line down, so a tap while the
// printer is not ready tests the cable instead of printing (W-1055). With no
// crossover plugged in that shows the fail, which is also the honest answer to
// "print a test page" with no printer ready.
static void test_button(void)
{
    on_activity();
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        int cancel = 0;
        portENTER_CRITICAL(&s_tap_mux);
        if (s_test_running) {
            // The write in progress is aborted; W-1041's resync runs before
            // the next job.
            s_test_cancel = 1;
            serial_out_set_abort(1);
            cancel = 1;
        }
        portEXIT_CRITICAL(&s_tap_mux);
        if (cancel) {
            ESP_LOGI(TAG, "test button tapped again: cancelling the test page");
        } else {
            ESP_LOGW(TAG, "test button tapped but the printer is busy");
            s_job_waiting = 1;          // an update in progress gives way (W-1048)
            led_grid_blip(LG_BLIP_BUSY);
        }
        return;
    }
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
    if (!serial_out_clear()) {
        ESP_LOGI(TAG, "test button with the printer not ready: testing the cable");
        if (xTaskCreate(cable_button_task, "cable", 4096, NULL, 3, NULL) != pdPASS)
            xSemaphoreGive(s_job_lock);
        return;
    }
#endif
    test_page_locked();
}

// Latched by dip_on_change from the boot-time read, consumed once app_main
// has brought the servers up.
static int s_test_on_boot;

static void lt_start(const char *why);

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
    s_dip = *cfg;
    s_dip_known = 1;
    // W-1079: once the page has set the AppleTalk port, its switch decides.
    if (cfg->mode == 2 && s_set.lt_port < 0) lt_start("DIP mode 2");
    else if (localtalk_running())
        ESP_LOGI(TAG, "dip: LocalTalk stays on until the board restarts");
}

// --------------------------------------------------------- factory reset
// W-1045: hold the test button. Past a tap (1 s) the panel counts down; at
// 8 s every setting is erased -- the point of no return -- and the board
// restarts once the button is let go. Not before: a button still down at
// restart reads to the bootloader as the start of its own power-up reset,
// which would also put the known-good firmware back (W-1046, W-1059).

#define RESET_RELEASE_WAIT_MS 30000     // a button stuck down still restarts

static volatile int s_reset_released;
static uint32_t s_hold_t0;

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void restart_when_released(void)
{
    for (uint32_t waited = 0; !s_reset_released && waited < RESET_RELEASE_WAIT_MS; waited += 50)
        vTaskDelay(pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void reset_task(void *arg)
{
    (void)arg;
    // Wi-Fi first, so the driver is not writing its own NVS keys while the
    // partition goes. Everything after this restarts anyway.
    esp_wifi_stop();
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        // Back on the network with whatever survived.
        ESP_LOGE(TAG, "factory reset: erasing settings failed (%s)", esp_err_to_name(err));
        led_grid_action(LG_ACT_RESET_FAILED, 0);
        vTaskDelay(pdMS_TO_TICKS(LG_RESET_FAILED_MS));
        restart_when_released();
    }
    ESP_LOGW(TAG, "factory reset: settings erased; restarting when the button is let go");
    led_grid_action(LG_ACT_RESET_DONE, 0);
    vTaskDelay(pdMS_TO_TICKS(LG_RESET_DONE_MS));
    restart_when_released();
}

// W-1079: the settings page's Restart, Erase All Settings and Print Test Page.
// Restart lets a printing page finish (up to its own 10-minute stall limit).
static void restart_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1300 && s_printing; i++) vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(300));                    // the reply goes out first
    esp_restart();
}

static void web_restart(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "restart from the settings page%s", s_printing ? ", after this page" : "");
    if (xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "restart: could not start");
}

// As the held button's reset, with nothing to wait for being let go.
static void web_erase(void *ctx)
{
    (void)ctx;
    ESP_LOGW(TAG, "factory reset from the settings page");
    s_reset_released = 1;
    if (xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "factory reset: could not start");
        led_grid_action(LG_ACT_RESET_FAILED, 0);
    }
}

static int web_test_page(void *ctx)
{
    (void)ctx;
    on_activity();
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) return 1;
    test_page_locked();
    return 0;
}

// On dip_config's poll task: start work, never wait on it.
static void on_hold(btn_ev_t ev)
{
    switch (ev) {
    case BTN_EV_HOLD:
        on_activity();
        s_hold_t0 = uptime_ms();
        led_grid_action(LG_ACT_RESET_HOLD, 0);
        ESP_LOGW(TAG, "test button held: factory reset in %us unless it is let go",
                 (unsigned)(BTN_FILL_MS / 1000));
        break;
    case BTN_EV_CANCEL: {
        uint32_t held = uptime_ms() - s_hold_t0;
        uint16_t fill = (uint16_t)(held >= BTN_FILL_MS ? 1000 : held * 1000 / BTN_FILL_MS);
        led_grid_action(LG_ACT_RESET_CANCEL, fill);
        ESP_LOGI(TAG, "test button let go: factory reset cancelled");
        break;
    }
    case BTN_EV_COMMIT:
        s_reset_released = 0;
        if (xTaskCreate(reset_task, "reset", 3072, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "factory reset: could not start");
            led_grid_action(LG_ACT_RESET_FAILED, 0);
        }
        break;
    case BTN_EV_RELEASE:
        s_reset_released = 1;
        break;
    default:
        break;
    }
}

// The bootloader erased the settings at power-up (test button held through
// it): show the reset's confirmation once the boot spin is over.
static volatile int s_boot_reset;

// ----------------------------------------------------------------- wifi

// An IPv4 address in host order (W-1076: the tunnel's MAC is made of it).
static uint32_t ip_host(const esp_ip4_addr_t *a)
{
    return (uint32_t)esp_ip4_addr1(a) << 24 | (uint32_t)esp_ip4_addr2(a) << 16 |
           (uint32_t)esp_ip4_addr3(a) << 8 | esp_ip4_addr4(a);
}

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
            ethertalk_net_down();                       // W-1076: its jobs end now
            printer_state_post(PS_EV_WIFI_DOWN);
            ota_health_set(OH_STA_UP, 0);
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
        ota_health_set(OH_STA_UP, 1);
        fw_update_net_up();
        ethertalk_ip_changed(ip_host(&e->ip_info.ip));     // the tunnel's MAC is the IP
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

// A string for a JSON value: quotes and backslashes escaped, control
// characters dropped. Network names and printer names are the user's.
static const char *json_esc(char *dst, size_t cap, const char *src)
{
    size_t k = 0;
    for (; src && *src && k + 2 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c < 0x20) continue;
        if (c == '"' || c == '\\') dst[k++] = '\\';
        dst[k++] = (char)c;
    }
    dst[k] = '\0';
    return dst;
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

    // W-1079: the DIP bank as switches 1-8, "1" for on; "" before the first read.
    char dip[9] = "";
    if (s_dip_known) {
        uint8_t on = (uint8_t)(s_dip.mode | s_dip.dev_mode << 2 | s_dip.test_on_boot << 3 |
                               s_dip.dither_ovr << 4 | s_dip.gamer << 6 | s_dip.spare << 7);
        for (int i = 0; i < 8; i++) dip[i] = (on >> i) & 1 ? '1' : '0';
        dip[8] = '\0';
    }
    char e_ssid[80], e_queue[80], e_job[200], e_model[80], e_name[80];

    char job[96];
    if (s_last_job[0])
        snprintf(job, sizeof job, "%s (%u page%s)", s_last_job,
                 (unsigned)s_last_pages, s_last_pages == 1 ? "" : "s");
    else
        snprintf(job, sizeof job, "none yet");

    snprintf(buf, cap,
        "{\"state\":\"%s\",\"ps\":\"%s\",\"asleep\":%s,\"stopped\":%s,\"setup\":%s,\"job\":\"%s\","
        "\"fault\":%s,\"printer\":%s,\"printing\":%s,\"ribbon\":\"%s\","
        "\"queue\":\"%s\",\"host\":\"%s.local\",\"ip\":\"%s\","
        "\"ssid\":\"%s\",\"dither\":\"%s\",\"gain\":%u,\"signal\":\"%s\","
        "\"channel\":\"%s\",\"security\":\"%s\",\"uri\":\"%s\","
        "\"uptime\":\"%uh %02um\",\"reset\":\"%s\",\"version\":\"%s\",\"heap\":\"%u KB\","
        "\"slot\":\"%s\",\"trial\":%s,\"rolled_back\":\"%s\","
        "\"known_good\":\"%s\",\"known_good_version\":\"%s\","
        "\"model\":\"Apple ImageWriter II\",\"pic\":%s,"
        "\"pmodel\":\"%s\",\"pname\":\"%s\",\"phost\":\"%s\",\"airprint\":%s,"
        "\"lt_port\":%d,\"lt\":%s,\"dip\":\"%s\"}",
        state_text(v.state),
        ps_name(v.state),
        v.asleep ? "true" : "false",
        attention || offline ? "true" : "false",
        s_ap_mode ? "true" : "false",
        json_esc(e_job, sizeof e_job, job),
        attention || failed ? "true" : "false",
        serial_out_clear() ? "true" : "false",
        s_printing ? "true" : "false", ribbon_name(s_ribbon),
        json_esc(e_queue, sizeof e_queue, s_set.queue), s_set.hostname, s_ip,
        json_esc(e_ssid, sizeof e_ssid, joined ? (char *)ap.ssid : (s_ap_mode ? "" : s_set.ssid)),
        s_set.dither, (unsigned)s_set.gain,
        joined ? (ap.rssi > -55 ? "Excellent" : ap.rssi > -67 ? "Good"
                : ap.rssi > -75 ? "Fair" : "Weak") : "",
        joined ? "2.4 GHz" : "",
        joined ? auth_name(ap.authmode) : "",
        s_ap_mode ? "" : ipp_server_uri(),
        (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
        reset_name(esp_reset_reason()), app->version, (unsigned)(esp_get_free_heap_size() / 1024),
        ota_health_slot(), ota_health_on_trial() ? "true" : "false", ota_health_rolled_back(),
        ota_health_known_good(), ota_health_known_good_version(),
        pic_prog_available() ? "true" : "false",
        json_esc(e_model, sizeof e_model, s_saved_model), json_esc(e_name, sizeof e_name, s_saved_name),
        s_saved_host, s_set.airprint ? "true" : "false", s_set.lt_port,
        localtalk_running() && !localtalk_quiet() ? "true" : "false", dip);
}

static void lt_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    localtalk_json(buf, cap);
}

static const uint8_t *lt_job(void *ctx, size_t *len)
{
    (void)ctx;
    return localtalk_job(len);
}

static void lt_echo(void *ctx, int node)
{
    (void)ctx;
    localtalk_echo((uint8_t)node);
}

// W-1076: the EtherTalk links on the web page.
static void et_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    ethertalk_json(buf, cap);
}

static void et_settings_get(void *ctx, int *tunnel, int *radio, int *infinite, char *zone, size_t cap)
{
    (void)ctx;
    ethertalk_settings(tunnel, radio, infinite, zone, cap);
}

static int et_settings_set(void *ctx, int tunnel, int radio, int infinite, const char *zone)
{
    (void)ctx;
    esp_err_t err = ethertalk_apply(tunnel, radio, infinite, zone);
    if (err == ESP_ERR_INVALID_ARG) return -1;
    if (err != ESP_OK) ESP_LOGE(TAG, "EtherTalk settings not saved: %s", esp_err_to_name(err));
    return 0;
}

// The printer's state as the LocalTalk Option Card's status word, which the
// Mac's driver reads with PAP SendStatus, for a node on `link`.
static uint16_t lt_status(ps_state_t st, at_link_t link)
{
    switch (st) {
    case PS_ATTENTION: return LT_IW_PAPER_OUT | LT_IW_OFFLINE | LT_IW_FAULT;
    case PS_OFFLINE:   return LT_IW_OFFLINE;
    // Busy is for someone else's job. A Mac told "busy" about its own sits on
    // the connection and never sends (W-1051 bench).
    case PS_PRINTING:  return at_job_link() == (int)link ? 0 : LT_IW_BUSY;
    default:           return 0;
    }
}

// W-1049: what the LocalTalk node saw, on the panel. A Chooser asks again
// every second or so while it is open, so "a Mac heard" shows at most every
// 6 s; each one still keeps the panel awake.
#define MAC_BLIP_EVERY_MS 6000

// Both the cable's node and the EtherTalk links (W-1076) report here.
static void lt_event(lt_event_t ev)
{
    static uint32_t last;
    if (ev == LT_EV_ONLINE) {
        led_grid_blip(LG_BLIP_LT_ON);
        return;
    }
    printer_state_post(PS_EV_ACTIVITY);
    uint32_t now = uptime_ms();
    if (last && now - last < MAC_BLIP_EVERY_MS) return;
    last = now;
    led_grid_blip(LG_BLIP_MAC);
}

// W-1051: the LocalTalk node, in DIP mode 2 or on a bench build. It owns its
// UART and the PIC's pins from here until reboot.
static void lt_start(const char *why)
{
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0 && CONFIG_AIRWRITER_PIC_DAT_PIN >= 0
    if (localtalk_running()) return;
    // A DIP flip mid cable test: let the test give UART2 back first.
    for (int i = 0; i < 30 && cable_test_lt_busy(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    localtalk_cfg_t c = {
        .uart_num = CONFIG_AIRWRITER_LT_UART_NUM,
        .tx_pin   = CONFIG_AIRWRITER_PIC_CLK_PIN,
        .rx_pin   = CONFIG_AIRWRITER_PIC_DAT_PIN,
        .cts_pin  = CONFIG_AIRWRITER_LT_CTS_PIN,
        .object   = s_set.queue,
        .type     = CONFIG_AIRWRITER_LT_NBP_TYPE,
        .status   = lt_status(printer_state_view().state, AT_LINK_CABLE),
        .printer  = { .begin = lt_print_begin, .write = lt_print_write, .end = lt_print_end },
        .on_event = lt_event,
    };
    esp_err_t err = localtalk_start(&c);
    if (err != ESP_OK) ESP_LOGE(TAG, "LocalTalk (%s) did not start: %s", why, esp_err_to_name(err));
    else               ESP_LOGI(TAG, "LocalTalk on (%s)", why);
#else
    (void)why;
#endif
}

// W-1079: the AppleTalk port's switch. Off quiets a running node (it cannot
// stop); on starts one, or wakes a quiet one.
static void lt_port_apply(int on)
{
    if (!on) {
        localtalk_set_quiet(1);
        return;
    }
    if (localtalk_running()) localtalk_set_quiet(0);
    else lt_start("the settings page");
}

// W-1076, W-1078: EtherTalk for Macs on this network, Basilisk II and
// Infinite Mac, whatever the DIP mode, once the station has an address. Each
// way in is a setting.
static void et_start(void)
{
    ps_state_t st = printer_state_view().state;
    esp_netif_ip_info_t ip = {0};
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_get_ip_info(sta, &ip);
    ethertalk_cfg_t c = {
        .object        = s_set.queue,
        .type          = CONFIG_AIRWRITER_LT_NBP_TYPE,
        .tunnel_status = lt_status(st, AT_LINK_TUNNEL),
        .zone_status   = lt_status(st, AT_LINK_ZONE),
        .radio_status  = lt_status(st, AT_LINK_RADIO),
        .ip            = ip_host(&ip.ip),
        .printer       = { .begin = lt_print_begin, .write = lt_print_write, .end = lt_print_end },
        .on_event      = lt_event,
    };
    esp_err_t err = ethertalk_start(&c);
    if (err != ESP_OK) ESP_LOGE(TAG, "EtherTalk did not start: %s", esp_err_to_name(err));
}

static void localtalk_flash(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    // The node holds the PIC's UART pins; flashing would fight it. "err", as
    // pic_result_json has it, is what the page shows.
    if (localtalk_running()) {
        snprintf(buf, cap, "{\"ok\":false,\"err\":\"LocalTalk is running: set DIP1-2 to "
                 "another mode and restart, then flash\"}");
        return;
    }
    // A cable test borrows the same pins, under the job lock.
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        snprintf(buf, cap, "{\"ok\":false,\"err\":\"The printer port is busy; try again "
                 "when the job is done\"}");
        return;
    }
    pic_result_t r;
    on_activity();
    led_grid_action(LG_ACT_PIC_FLASHING, 0);
    pic_err_t err = pic_prog_flash_tashtalk(&r);
    xSemaphoreGive(s_job_lock);
    led_grid_action(err == PIC_OK ? LG_ACT_PIC_OK : LG_ACT_PIC_FAILED, 0);
    pic_result_json(&r, buf, cap);
}

// POST /api/cable. Refuses rather than waits while a job holds the port.
static void cable_web(void *ctx, int both, char *buf, size_t cap)
{
    (void)ctx;
    (void)both;
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        snprintf(buf, cap, "{\"ok\":false,\"error\":\"The printer port is busy; try again "
                 "when the job is done\"}");
        return;
    }
    on_activity();
    cable_result_t r;
    cable_test_locked(&r, both);
    xSemaphoreGive(s_job_lock);
    cable_result_json(&r, buf, cap);
#else
    snprintf(buf, cap, "{\"ok\":false,\"error\":\"no LocalTalk port\"}");
#endif
}

// ----------------------------------------------------------------- main

// Runs on printer_state's poll task, once per change of display state.
static void on_ps_change(const ps_view_t *v)
{
    ESP_LOGI(TAG, "state -> %s", ps_name(v->state));
    if (s_boot_reset && v->state != PS_BOOTING) {
        s_boot_reset = 0;
        led_grid_action(LG_ACT_RESET_CONFIRM, 0);
    }
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
    localtalk_set_status(lt_status(v->state, AT_LINK_CABLE));
    ethertalk_set_status(lt_status(v->state, AT_LINK_TUNNEL), lt_status(v->state, AT_LINK_ZONE),
                         lt_status(v->state, AT_LINK_RADIO));
}

// ------------------------------------------------------- firmware updates
// fw_update borrows the job lock: an update holds it to the restart (W-1048).

static int     fwu_try_lock(void)   { return xSemaphoreTake(s_job_lock, 0) == pdTRUE; }
static void    fwu_unlock(void)     { xSemaphoreGive(s_job_lock); }
static int     fwu_waiting(void)    { return s_job_waiting; }
static void    fwu_wait_clear(void) { s_job_waiting = 0; }
static int64_t fwu_last_job(void)   { return s_last_job_s; }

static void fwu_start(int setup_mode)
{
    fw_update_cfg_t fc = {
        .try_lock = fwu_try_lock, .unlock = fwu_unlock,
        .job_waiting = fwu_waiting, .job_waiting_clear = fwu_wait_clear,
        .last_job_s = fwu_last_job, .setup_mode = setup_mode, .auto_on = s_set.auto_update,
    };
    ESP_ERROR_CHECK(fw_update_start(&fc));
}

void app_main(void)
{
    // Before anything else: U9's level-shifter input has no pull, so from
    // power-on until this line the LED chain sees noise (a brief flicker at
    // boot is expected on rev A; rev B adds a 100k pulldown on U9.2). It must
    // run before wifi_start() especially -- the join loop can block for 60s.
    // Compile-time guard: the pin is -1 on a devkit with no strip.
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
    // the default is -1; a devkit wired to a breadboard PIC sets its own pin
    // and gets the same clamp.
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
    tm.boot_out_ms = LG_SPIN_OUT_MS;
    printer_state_start(&tm, on_ps_change);
    // After the clamp above: the RMT driver takes IO48 over from here, and
    // by the first frame the line is driven, never floating.
    ESP_ERROR_CHECK(led_grid_start());

    ESP_LOGI(TAG, "AirWriter starting after %s reset, %u bytes free",
             reset_name(esp_reset_reason()), (unsigned)esp_get_free_heap_size());
    if (bootloader_common_get_rtc_retain_mem_factory_reset_state()) {
        ESP_LOGW(TAG, "power-up reset by the bootloader: settings erased, known-good firmware");
        s_boot_reset = 1;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Default hostname imagewriter-XXXX, the last two MAC bytes. SPEC.md 3.2.
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_suffix, sizeof s_suffix, "%02x%02x", mac[4], mac[5]);
    settings_load(&s_set, s_suffix);
    s_ribbon = s_set.ribbon;
    // Wi-Fi saved before this restart: green greets the join (W-1049).
    if (settings_take_wifi_fresh()) printer_state_post(PS_EV_WIFI_FRESH);
    ota_health_start(s_set.ssid[0] != 0);
    snprintf(s_saved_model, sizeof s_saved_model, "%s", s_set.model);
    snprintf(s_saved_name, sizeof s_saved_name, "%s", s_set.name);
    snprintf(s_saved_host, sizeof s_saved_host, "%s", s_set.hostname);
#if CONFIG_AIRWRITER_LT_BENCH
    lt_start("bench build");
    if (s_set.lt_port == 0) localtalk_set_quiet(1);
#endif
    if (s_set.lt_port == 1) lt_start("the settings page");

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
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
    cable_test_cfg_t ct = {
        .tx_pin  = CONFIG_AIRWRITER_TX_PIN,       .rx_pin = CONFIG_AIRWRITER_RX_PIN,
        .invert_tx = AW_INVERT_TX,
        .lt_uart = CONFIG_AIRWRITER_LT_UART_NUM,  .lt_tx  = CONFIG_AIRWRITER_PIC_CLK_PIN,
        .lt_rx   = CONFIG_AIRWRITER_PIC_DAT_PIN,  .lt_cts = CONFIG_AIRWRITER_LT_CTS_PIN,
        .lt_mclr = CONFIG_AIRWRITER_PIC_MCLR_PIN,
    };
    cable_test_init(&ct);
#endif
    // 4 KB: a ribbon change saves to NVS and re-announces mDNS from here.
    xTaskCreate(kick_task, "kick", 4096, NULL, 3, NULL);

    job_pipe_cfg_t pipe = { .out = printer_out, .dither = algo, .gain = s_set.gain,
                            .done = pipe_done, .colour_ok = colour_ok };
    ESP_ERROR_CHECK(job_pipe_init(&pipe));

    // Before wifi_start, which can block up to 60s joining: the debounce
    // settles in ~300ms, so the boot-time read is long ready by the time
    // test_on_boot is checked below, whichever path gets there.
    dip_config_cb_t dip_cb = { .on_change = dip_on_change, .on_test_button = test_button,
                               .on_hold = on_hold };
    ESP_ERROR_CHECK(dip_config_start(&dip_cb));

    // No credentials, or they no longer work: become an access point and let
    // the user fix it from a phone. This is the only path a person who did not
    // build the firmware can take.
    // Joining only when there is a network to try: setup mode after it is a
    // failed join, which the panel opens with two red pulses (W-1049).
    if (s_set.ssid[0]) printer_state_post(PS_EV_WIFI_JOINING);
    wifi_init();
    if (!s_set.ssid[0] || wifi_start() != ESP_OK) {
        char ap[32];
        snprintf(ap, sizeof ap, "AirWriter-%s", s_suffix);
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
        ota_health_set(OH_AP_UP, 1);
        fwu_start(1);
        webui_cfg_t wcfg = { .status_json = status_json, .general_saved = general_saved, .captive = 1,
                              .restart = web_restart, .erase = web_erase, .test_page = web_test_page,
                              .localtalk_flash = localtalk_flash,
                              .lt_json = lt_json, .lt_job = lt_job, .lt_echo = lt_echo,
                              .cable_test = cable_web };   // no EtherTalk: the zone stays off the open AP
        ESP_ERROR_CHECK(webui_start(&wcfg));
        ota_health_set(OH_WEB_UP, 1);

        for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
    }

    ipp_server_cfg_t cfg = {
        .queue_name = s_set.queue,
        .hostname   = s_set.hostname,
        .port       = 631,
        .dev_mode   = AW_DEV_MODE,
        .on_job_op  = on_activity,
        .colour     = s_ribbon == IW_RIBBON_COLOUR,
        .sink = { .begin = doc_begin, .data = doc_data, .end = doc_end,
                  .cancel = doc_cancel, .ctx = NULL },
    };
    ipp_server_set_airprint(s_set.airprint);          // W-1079
    ESP_ERROR_CHECK(ipp_server_start(&cfg));
    // The ribbon may have been read while the server started; push what it is.
    ipp_server_set_colour(s_ribbon == IW_RIBBON_COLOUR);
    ota_health_set(OH_IPP_UP, 1);
    // wifi_start returned joined, before this could hear the join.
    fwu_start(0);
    fw_update_net_up();

    webui_cfg_t wcfg = { .status_json = status_json, .general_saved = general_saved, .captive = 0,
                          .restart = web_restart, .erase = web_erase, .test_page = web_test_page,
                          .localtalk_flash = localtalk_flash,
                          .lt_json = lt_json, .lt_job = lt_job, .lt_echo = lt_echo,
                          .cable_test = cable_web, .et_json = et_json,
                          .et_settings_get = et_settings_get, .et_settings_set = et_settings_set };
    ESP_ERROR_CHECK(webui_start(&wcfg));
    ota_health_set(OH_WEB_UP, 1);
    et_start();

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
