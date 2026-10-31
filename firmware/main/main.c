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
#include "laser.h"
#include "printers.h"
#include "printer_caps.h"
#include "port_policy.h"
#include "stylewriter.h"
#include "sw_status.h"
#include "ota_health.h"
#include "fw_update.h"
#include "cable_test.h"
#include "testpage.h"
#include "esp_heap_caps.h"
#include "bigmem.h"
#include "memdiag.h"
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
// The test page holds s_job_lock, and a second tap cancels it (W-1049). The
// tap and the page's end decide under s_tap_mux, so a tap landing as the page
// finishes can never set its cancel on the next job instead.
static portMUX_TYPE s_tap_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_test_running;      // the test page holds the lock
static int s_sw_test_running;   // ... and it is a StyleWriter's: a session, not ImageWriter writes
static volatile int s_test_cancel;  // a second tap asked to stop it

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

    // Once at boot: from the heap, not 2.3 KB of .bss held for good (W-1094).
    iw_t *w = big_calloc(1, sizeof *w);
    if (!w) {
        ht_free(&h);
        ESP_LOGE(TAG, "raster selftest: no memory");
        return;
    }
    size_t emitted = 0;
    iw_init(w, SELFTEST_W, count_out, &emitted);
    iw_begin_page(w);

    uint8_t gray[SELFTEST_W];
    uint8_t bits[(SELFTEST_W + 7) / 8];
    for (uint32_t y = 0; y < SELFTEST_H; y++) {
        for (uint32_t x = 0; x < SELFTEST_W; x++)
            gray[x] = (uint8_t)(x * 255 / (SELFTEST_W - 1));
        ht_row(&h, gray, bits);
        iw_row(w, bits);
    }

    iw_end_page(w);
    free(w);
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
// cancelled. Abort the job so the rip task winds down and job_pipe drops the
// rest of the job's bytes instead of pushing them at a dead line. On the
// printer task, the one writing.
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

// An IPP job holds s_job_lock: from doc_claim's take to job_finished's give.
// A Cancel-Job before doc_begin must not reach the printer another job owns
// -- a LocalTalk job holds it for minutes (W-1053 review). ipp_server passes
// the cancel on again once begin returns.
static volatile int s_ipp_owns;

// The printer stream: checksummed so the device can be compared with tools/rip
// byte for byte, then put on the wire.
// The last stream left the paper away from top of form (cut, or a Mac's that
// stopped mid-page): a form feed first, so this job starts on a fresh page.
// Outside the CRC, as the resync is; after it, in text mode.
static esp_err_t paper_to_tof(void)
{
    if (!iw_link_need_ff(&s_link) || AW_LOOPBACK) return ESP_OK;
    ESP_LOGW(TAG, "the paper was left mid-page; a form feed first");
    static const uint8_t ff[] = { 0x0C };
    return serial_out_write(ff, sizeof ff);
}

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
    if (paper_to_tof() != ESP_OK) { printer_fail(); return; }
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
// W-1082: which printer the panel's printer line follows. While a LaserWriter
// job prints, or with no ImageWriter shared and a LaserWriter shared, the
// LaserWriter's readiness (laser_ready); while the port prints, its HSKo; idle,
// the box's (W-1088): ready while any shared printer is (box_ready).
enum { LINE_IW, LINE_LASER };
static volatile int        s_line_src = LINE_IW;
static volatile int        s_iw_ready = -1;    // HSKo as last reported, -1 before
// W-1116: an ImageWriter record reads Ready only once ESC ? has named its model since the
// line came up, never from the line alone, so the page never shows the wrong ImageWriter for a
// moment. Cleared on a line-up after a second or more down, set when ESC ? names the current
// record's model (or an original ImageWriter record's silent line).
static volatile int        s_iw_named;
static volatile int        s_laser_owns;       // the job in the pipeline is the LaserWriter's

// W-1103: a StyleWriter on the port. Its HSKo says nothing, so its own answers are its state:
// `s_iw_ready` above is then what they say (-1 before the first), not the line.
static volatile int        s_port_sw;          // the port's record is a StyleWriter (printers_changed)
static volatile int        s_port_answering;   // the port's printer answered its last question
static volatile sw_state_t s_sw_state = SWS_OFFLINE;   // and what it said (a job's session also reports it)
static volatile int        s_sw_cart;          // pr_live_t.cart: 0 not read yet, 1 black, 2 colour
static volatile int        s_sw_owns;          // the job in the pipeline is the StyleWriter's
static volatile int        s_hsko = -1;        // HSKo as last reported, whichever printer is there
static volatile uint32_t   s_line_drops;       // and how often it has gone down
// Events for the port task (kick_task), set by whoever knows and cleared by what acts on them.
static volatile int        s_look;             // boot, setup or the Printers section asked
static volatile int        s_confirm_sw;       // the person named the port's printer a StyleWriter
static volatile int        s_confirm_sw_id;    // ...the record they picked (pr_port_sw_found)
static volatile int        s_link_check;       // the record's kind changed: J2's link may be the wrong one
static volatile int        s_screen_check;     // the record on the port changed under a job: its screen follows after
// The port task's books between its calls of port_policy (port_policy.h's port_in_t).
static uint32_t            s_last_plain_ms, s_last_status_ms;
static int                 s_plain_asked, s_plain_unanswered, s_event_plain, s_full_due;
// W-1116: an act to repeat at once (port_in_t.confirm), set after a seen call that returned
// PR_SEEN_AGAIN (one sighting of a model with no record; the same answer twice makes it).
static port_act_t          s_port_confirm;
// W-1116: the last printers_port_seen result ribbon_ask kept, for the port task: the record id
// the ESC ? answer belongs to, PR_SEEN_AGAIN, or -1.
static int                 s_seen_again = -1;
static volatile int        s_look_person;      // the pending look is the person's Look for Printers
// The port's StyleWriter answered before the session began (a job, the test page, a cleaning):
// if the session then gets nothing, the port is looked at once when it is over.
static volatile int        s_sw_was_answering;
// A session (a job, the test page, a cleaning, the person's pick) got no answer from the port's
// StyleWriter since it last answered: the original then reads as offline when idle and silent
// (sw_idle_silent), until it answers or the next session begins.
static volatile int        s_sw_job_silent;
static int                 s_port_refused;     // the port's job was refused at its begin: another printer is there
// W-1116: the AirPrint printer (slot) the port's job came in on, from the sink's ctx: its
// record's, which may be any slot. The job's end goes back to it.
static int                 s_port_job_slot;
static uint32_t uptime_ms(void);
static void port_push(ps_state_t box);

static char s_suffix[8];            // the MAC's last two bytes, the default hostname's

// W-1079: what the page last saved, beside s_set's names in use since boot:
// a new name or hostname reaches mDNS and the Chooser only at a restart.
static char s_saved_model[SET_QUEUE_MAX], s_saved_name[SET_QUEUE_MAX], s_saved_host[SET_HOST_MAX];
// The DIP bank as last read, for the page (W-1079). Unknown until the first.
static dip_config_t s_dip;
static volatile int s_dip_known;

// W-1081: whether U7 runs TashTalk, for the AppleTalk Port card: asked at
// boot, whenever the node starts, and after a flash. Unknown until then.
typedef enum { LT_CHIP_UNKNOWN, LT_CHIP_OK, LT_CHIP_NONE, LT_CHIP_NO_TASHTALK,
               LT_CHIP_SILENT } lt_chip_t;
static volatile lt_chip_t s_lt_chip;

static const char *lt_chip_name(lt_chip_t c)
{
    switch (c) {
    case LT_CHIP_OK:          return "ok";
    case LT_CHIP_NONE:        return "no_chip";
    case LT_CHIP_NO_TASHTALK: return "no_tashtalk";
    case LT_CHIP_SILENT:      return "silent";
    default:                  return "unknown";
    }
}

// The page saved queue, hostname, dither or gain (W-1068): re-read them, and
// give the pipe and the test page the screen they now name. The hostname
// and queue still take a restart to reach mDNS, as they always have.
static void lt_port_apply(int on);
static void line_source(void);

static void general_saved(void *ctx)
{
    (void)ctx;
    settings_t fresh;
    if (settings_load(&fresh, s_suffix) != ESP_OK) return;
    // W-1088: the port printer's record holds its screen; the old Printer
    // pane's save reaches it here.
    if (strcmp(fresh.dither, s_set.dither) || fresh.gain != s_set.gain)
        printers_port_set_screen(fresh.dither, fresh.gain);
    strncpy(s_set.dither, fresh.dither, sizeof s_set.dither - 1);
    s_set.gain = fresh.gain;
    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;
    job_pipe_set_screen(algo, s_set.gain);
    ESP_LOGI(TAG, "screen now %s, gain %u%%", s_set.dither, s_set.gain);
    if (fresh.top_margin != s_set.top_margin) {
        s_set.top_margin = fresh.top_margin;
        job_pipe_set_top_margin(s_set.top_margin);
        ESP_LOGI(TAG, "top margin now %d/144 in", s_set.top_margin);
    }
    // W-1079: the names wait for a restart; the switches act now.
    snprintf(s_saved_model, sizeof s_saved_model, "%s", fresh.model);
    snprintf(s_saved_name, sizeof s_saved_name, "%s", fresh.name);
    snprintf(s_saved_host, sizeof s_saved_host, "%s", fresh.hostname);
    if (fresh.airprint != s_set.airprint) {
        s_set.airprint = fresh.airprint;
        ipp_server_set_airprint(s_set.airprint);
        line_source();                       // W-1082: no ImageWriter, the LaserWriter's lights
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
// its first write would have sent. 1 when the printer answered as an ImageWriter.
static int ribbon_ask(int for_job)
{
    if (AW_LOOPBACK) return 0;
    s_seen_again = -1;                  // no answer unless printers_port_seen is reached below
    // W-1088: ESC ? also finds what is on the port, so it goes to anything
    // but a printer the person said is something else; a job's goes only to
    // an ImageWriter.
    port_mode_t mode = printers_port_mode();
    // W-1116: on a Something else port only the person's own Look for Printers asks (the policy
    // sends PA_ESCQ for that alone); a job's goes only to an ImageWriter.
    if (mode == PORT_UNKNOWN_SAID && (for_job || !s_look_person)) return 0;
    if (for_job && mode != PORT_IMAGEWRITER) return 0;
    for (uint32_t ms = 0; for_job && !serial_out_clear() && !s_job_cancelled && !s_test_cancel &&
                          ms < CONFIG_AIRWRITER_STALL_ABORT * 1000u; ms += 100)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (!serial_out_clear()) return 0;
    if (s_link.dirty) {
        if (!for_job) return 0;
        esp_err_t err = ESP_OK;
        iw_resync(resync_out, &err);
        if (err != ESP_OK) return 0;
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
    if (n < 0) return 0;
    char hex[3 * sizeof reply + 1] = "";
    for (int i = 0; i < n; i++) snprintf(hex + 3 * i, 4, " %02x", reply[i]);
    iw_self_id_t id = iw_self_id(reply, (size_t)n);
    iw_ribbon_t r = id.ribbon;
    ESP_LOGI(TAG, "self-ID: %d byte%s%s, %s, %s ribbon", n, n == 1 ? "" : "s", hex,
             id.model == IW_MODEL_LQ ? "LQ" : id.model == IW_MODEL_II ? "II" : "no model",
             ribbon_name(r));
    s_seen_again = printers_port_seen(id.model, 1);
    int answered = id.model != IW_MODEL_NONE;
    // The ImageWriter on the port named its model, or an original ImageWriter record answered
    // with its silent line (the core then makes it current): Ready may now show for it. A
    // first sighting (PR_SEEN_AGAIN), and silence on a port whose record is a II or LQ (a lost
    // answer: the core keeps the record and nothing is named), leave Ready waiting.
    if (s_seen_again > 0 && printers_port_is_imagewriter() &&
        (answered || printers_port_model() == PR_MODEL_IW1) && !s_iw_named) {
        s_iw_named = 1;
        line_source();                  // the panel follows the record's Ready (box_ready)
    }
    if (!printers_port_is_imagewriter() || r == IW_RIBBON_UNKNOWN || r == s_ribbon) return answered;
    ESP_LOGI(TAG, "ribbon now %s (was %s)", ribbon_name(r), ribbon_name(s_ribbon));
    s_ribbon = (uint8_t)r;
    settings_save_ribbon((uint8_t)r);
    port_push(printer_state_view().state);      // the colour it offers (W-1116: its own slot)
    return answered;
}

// ESC ? is the ImageWriter's, at 9600 with CTS (W-1103): a StyleWriter's link is given
// up for it and taken back, so a port whose record is a StyleWriter that has stopped
// answering can still be asked whether an ImageWriter has taken its place. Before a job's
// or the test page's ImageWriter bytes the link stays the ImageWriter's, whatever the record
// becomes meanwhile.
static int ribbon_check(int for_job)
{
    stylewriter_link(0);
    int answered = ribbon_ask(for_job);
    if (!for_job) stylewriter_link_record();
    return answered;
}

// job_pipe asks as each job's rip starts, on the pipeline task, while the
// job holds s_job_lock.
static int colour_ok(void *ctx)
{
    (void)ctx;
    if (!printers_port_is_imagewriter()) return 0;
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
        // A fresh line is not Ready until ESC ? names the model (W-1116). Not mid-job (a paper
        // reload drops the line), and not while the link is dirty after a job cut short (the
        // idle ESC ? waits for a job's resync, so the record it named is kept, ruling R17; the
        // plain question still runs).
        if (!s_printing && !s_link.dirty) s_iw_named = 0;
    }
    s_line_seen = 1;
    s_hsko = ready;                     // the port task's, for any record
    if (!ready) s_line_drops++;
    // W-1103: a StyleWriter's line says nothing; its answers are its state, and the
    // line only tells the port task when an ImageWriter may have taken its place.
    if (s_port_sw) return;
    s_iw_ready = ready;
    printer_state_port_line(ready);     // the kick's, whatever line the panel follows
    // Mid-job the line toggles every printed line: this stays cheap.
    if (s_line_src == LINE_IW && s_printing) {
        printer_state_post(ready ? PS_EV_LINE_READY : PS_EV_LINE_NOT_READY);
        return;
    }
    if (s_line_src == LINE_IW) line_source();
    // Idle, the box may not change while the port does (W-1088): its printer's own state.
    if (!s_printing) port_push(printer_state_view().state);
}

// W-1088: the box's line while idle. Ready while any shared printer is; with
// nothing shared, the port's line, as before. Unknown (-1) until the port's
// ImageWriter, when shared, has reported, so the quiet power-on's Spin waits.
static int box_ready(void)
{
    int iw = printers_port_shared(), lw = laser_ready();   // lw < 0: none shared
    if (!iw && lw < 0) return s_iw_ready;
    // W-1116: an ImageWriter record is ready once ESC ? has named it, as port_live reads it,
    // so the panel does not read Ready before the record does.
    int iw_up = s_iw_ready == 1 && (s_port_sw || s_iw_named);
    if ((iw && iw_up) || lw == 1) return 1;
    if (iw && s_iw_ready < 0) return -1;
    return 0;
}

// W-1082: the line the panel follows, chosen again whenever a job starts or
// ends, the printer list changes, or a LaserWriter's readiness changes. Its
// state is posted at once (the panel takes a repeat as nothing new); before a
// LaserWriter has answered there is none, so the quiet power-on's Spin waits
// for it, to its 30 s cap.
static void line_source(void)
{
    int src = s_laser_owns || (!printers_port_shared() && laser_shared()) ? LINE_LASER : LINE_IW;
    s_line_src = src;
    int ready = src == LINE_LASER ? laser_ready() : s_printing ? s_iw_ready : box_ready();
    if (ready >= 0) printer_state_post(ready ? PS_EV_LINE_READY : PS_EV_LINE_NOT_READY);
}

// Both AirPrint sinks' claim: a job never waits out a Mac's job, the test page
// or an update with its document unread (ipp_server.h). Its client is told
// busy and asks again, as a Mac on AppleTalk is.
static int doc_claim(void *ctx)
{
    (void)ctx;
    if (xSemaphoreTake(s_job_lock, 0) == pdTRUE) return 1;
    s_job_waiting = 1;                  // an update in progress gives way (W-1048)
    return 0;
}

static esp_err_t sw_doc_begin(const char *format, const char *name, const ipp_job_opts_t *opts);
static void port_sw_sync(int sw, int locked);

static esp_err_t doc_begin(void *ctx, const char *format, const char *name,
                           const ipp_job_opts_t *opts)
{
    // W-1116: whose job, by its slot (printers.c's slot_ctx); the record id is the low byte,
    // for the job's own gate (a later commit).
    s_port_job_slot = (int)((intptr_t)ctx >> 8);
    // doc_claim took s_job_lock. ipp_server always pairs this call with
    // exactly one doc_end -- including when job_pipe_begin below fails -- and
    // every doc_end leads to exactly one job_finished, whose give balances
    // the claim, refusals included.
    // W-1103: whose job this is follows the record as it is now (a change made during the
    // last job waited for its end), and a port where the other family answered last hears
    // neither language: the job is refused, and the phone is told why (port_push).
    port_sw_sync(printers_port_is_stylewriter(), 1);
    if (printers_port_conflict()) {
        ESP_LOGW(TAG, "job \"%s\" refused: another printer answered on the printer port", name);
        s_port_refused = 1;
        s_job_pipe_ok = 0;
        port_push(printer_state_view().state);
        return ESP_FAIL;
    }
    s_ipp_owns = 1;
    // W-1103: the port's printer is a StyleWriter: the same sink, its own job.
    if (s_port_sw) return sw_doc_begin(format, name, opts);
    stylewriter_link(0);                // the ImageWriter's link, whatever the last job's was
    // The link's strength with each job: a weak one is what a slow upload
    // usually is (W-1042 bench).
    wifi_ap_record_t ap;
    int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    ESP_LOGI(TAG, "job \"%s\" format %s, rssi %d", name,
             format[0] ? format : "(unspecified)", rssi);
    memdiag_mark("job start");          // W-1094
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
    line_source();                  // the port's own line, not the box's
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
    if (s_sw_owns) stylewriter_cancel();     // its session stops at its next step
    else serial_out_set_abort(1);    // a paused printer must not hold the job hostage
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
    memdiag_mark("job done");           // W-1094: the low since its start

    // Clean only if the rip finished every page and every byte was queued;
    // anything else may have stopped mid-strip.
    int clean = st->rc == 0 && ok && !s_printer_failed;

    if (!clean) {
        iw_link_job_end(&s_link, 0);
        // The rip may have stopped between writes, leaving up to a FIFO of
        // the dead job queued behind a paused printer. The printer task has
        // stopped writing, or no write ever ran.
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
        ipp_server_job_done_for(s_port_job_slot, 0);
        return;
    }

    s_last_pages = (size_t)st->pages;
    s_last_bytes = st->printer_bytes;
    ESP_LOGI(TAG, "ripped %d page%s, %ux%u dots, %u bytes in -> %u printer bytes in %ums, "
             "all out in %ums", st->pages, st->pages == 1 ? "" : "s", (unsigned)st->dst_w,
             (unsigned)st->dst_h, (unsigned)st->doc_bytes, (unsigned)st->printer_bytes,
             (unsigned)st->rip_ms, (unsigned)st->ms);
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
        ipp_server_job_done_for(s_port_job_slot, 0);
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
    ipp_server_job_done_for(s_port_job_slot, 1);
}

static void laser_finished(const job_pipe_stats_t *st, int ok);
static void sw_finished(const job_pipe_stats_t *st, int ok);

// job_pipe's done callback: the pipeline has printed the document or let it go.
static void pipe_done(void *ctx, const job_pipe_stats_t *st)
{
    (void)ctx;
    if (s_sw_owns) {
        sw_finished(st, s_doc_ok);
    } else if (s_laser_owns) {
        laser_finished(st, s_doc_ok);
    } else {
        job_finished(st, s_doc_ok);
        printers_apply();           // W-1088: a change to the port's printer waited for it
        line_source();              // idle: the box's line
    }
}

// The upload is over; the printing usually is not. Returns at once so the
// client gets its answer while the pipeline prints from the job buffer, and
// pipe_done finishes the job when the paper stops.
static void doc_end(void *ctx, int ok)
{
    (void)ctx;
    if (s_port_refused) {               // nothing began: the claim's lock goes back, the job aborts
        s_port_refused = 0;
        xSemaphoreGive(s_job_lock);
        ipp_server_job_done_for(s_port_job_slot, 0);
        return;
    }
    s_doc_ok = ok;
    // A cut transfer (a client that cancels by dropping the upload rather
    // than with Cancel-Job) means nothing more will arrive: stop a write
    // that is waiting out a paused printer, so the pipeline unwinds now
    // instead of after the stall limit. A StyleWriter's session stops at its next step.
    if (!ok) {
        if (s_sw_owns) stylewriter_cancel();
        else serial_out_set_abort(1);
    }
    if (s_job_pipe_ok) {
        job_pipe_end(ok);
        return;
    }
    if (s_sw_owns) {                    // the pipeline never started: finish here
        job_pipe_stats_t none = {0};
        sw_finished(&none, 0);
        return;
    }
    // A failed job_pipe_begin never created the pipeline task, so nothing
    // will call pipe_done: finish here, with zeroed stats. ipp_server still
    // calls doc_end exactly once after a failed begin, so the give in
    // job_finished balances doc_begin's take.
    job_pipe_stats_t st = {0};
    job_finished(&st, 0);
    printers_apply();
    line_source();
}

// ------------------------------------------------------------ LaserWriter jobs
// W-1082: a shared LaserWriter on AppleTalk, AirPrint printer 1-6 (W-1088, W-1116). Its
// job takes the same job lock as the ImageWriter's (one job buffer, one rip),
// rips at 300 dpi into laser.c's PostScript and PAP client, and is a job like
// any other to the panel and to Macs on AppleTalk (told busy). The rip runs
// through job_pipe as the ImageWriter's does; laser_job_end waits for the
// printer. The sink's ctx is (slot << 8 | record id), the printer list's.

static int s_laser_pipe_ok;
static int s_laser_slot = 1;

static esp_err_t laser_doc_begin(void *ctx, const char *format, const char *name,
                                 const ipp_job_opts_t *opts)
{
    (void)opts;
    int id = (int)((intptr_t)ctx & 0xFF);
    s_laser_owns = 1;                   // doc_claim took s_job_lock
    memdiag_mark("job start");          // W-1094
    s_laser_slot = (int)((intptr_t)ctx >> 8);
    ESP_LOGI(TAG, "LaserWriter job \"%s\" for printer %d format %s", name, id,
             format[0] ? format : "(unspecified)");
    strncpy(s_last_job, name && name[0] ? name : "Untitled", sizeof s_last_job - 1);
    s_last_job[sizeof s_last_job - 1] = '\0';
    s_job_cancelled = 0;
    s_printing = 1;
    job_touch();
    led_grid_set_source(LG_SRC_AIRPRINT);
    line_source();
    printer_state_post(PS_EV_JOB_START);
    job_pipe_target_t t = { 0 };
    s_laser_pipe_ok = 0;
    if (laser_job_begin(id, name, &t)) return ESP_FAIL;
    esp_err_t err = job_pipe_begin_to(name, &t);
    s_laser_pipe_ok = err == ESP_OK;
    return err;
}

static void laser_doc_cancel(void *ctx)
{
    (void)ctx;
    if (!s_laser_owns) return;
    s_job_cancelled = 1;
    laser_job_cancel();
    job_pipe_abort();
}

// The rip is over; the printer usually is not. On the pipeline task (or the
// IPP worker, when the pipeline never started), as job_finished.
static void laser_finished(const job_pipe_stats_t *st, int ok)
{
    int printed = laser_job_end(st->rc == 0 && ok && !s_job_cancelled);
    s_printing = 0;
    job_touch();
    memdiag_mark("job done");           // W-1094
    s_last_pages = (size_t)st->pages;
    s_last_bytes = 0;
    printer_state_post(printed ? PS_EV_JOB_DONE : s_job_cancelled ? PS_EV_JOB_CANCELLED
                                                                  : PS_EV_JOB_FAILED);
    s_laser_owns = 0;
    line_source();
    xSemaphoreGive(s_job_lock);
    ipp_server_job_done_for(s_laser_slot, printed);
    printers_apply();               // W-1088: a change to its slot waited for it
}

static void laser_doc_end(void *ctx, int ok)
{
    (void)ctx;
    s_doc_ok = ok;
    if (!ok) laser_job_cancel();
    if (s_laser_pipe_ok) {
        job_pipe_end(ok);
        return;
    }
    job_pipe_stats_t st = {0};
    laser_finished(&st, 0);
}

// laser.c's view changed: the panel's line may follow it.
static void laser_changed(void)
{
    line_source();
}

// laser.c heard a printer, or its query ended: the printer list's.
static void laser_found(int link, const uint8_t *name, const uint8_t *type)
{
    printers_at_seen(link, name, type);
}

static void laser_queried(int id, int ok, const lw_info_t *info, const laser_at_t *at)
{
    printers_at_queried(id, ok, info, at ? at->link : -1, at ? at->net : 0, at ? at->node : 0,
                        at ? at->pages : -1);
}

// W-1088: a Mac's job relayed to a printer on another AppleTalk link holds the
// job lock as any job does, never waiting (the Mac hears busy and asks again),
// and shows on the panel in the Mac's stripes.
static int laser_relay(int take)
{
    if (take) {
        if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
            s_job_waiting = 1;
            return -1;
        }
        strncpy(s_last_job, "Relayed job", sizeof s_last_job - 1);
        s_printing = 1;
        s_laser_owns = 1;
        job_touch();
        led_grid_set_source(LG_SRC_LOCALTALK);
        line_source();
        printer_state_post(PS_EV_JOB_START);
        return 0;
    }
    printer_state_post(PS_EV_JOB_DONE);
    s_printing = 0;
    s_laser_owns = 0;
    job_touch();
    line_source();
    xSemaphoreGive(s_job_lock);
    return 0;
}

// ------------------------------------------------------------ StyleWriter jobs
// W-1103: the port's printer is a StyleWriter. Its job takes the same sink, job lock and
// pipeline as the ImageWriter's, rips at 360 dpi into sw_page's frames and goes out through
// a session (stylewriter.c) on the printer task: the printer is reset and its cartridge
// read as the rip begins, then each frame is sent as the printer's buffer has room.

static esp_err_t sw_doc_begin(const char *format, const char *name, const ipp_job_opts_t *opts)
{
    s_sw_owns = 1;                      // doc_claim took s_job_lock; sw_finished gives it
    s_sw_job_silent = 0;                // a new session: what it finds is the state
    s_sw_was_answering = s_port_answering;
    sw_model_t m = printers_port_sw_model();
    ESP_LOGI(TAG, "StyleWriter job \"%s\" format %s, quality %d", name,
             format[0] ? format : "(unspecified)", opts ? opts->quality : 0);
    memdiag_mark("job start");          // W-1094
    strncpy(s_last_job, name && name[0] ? name : "Untitled", sizeof s_last_job - 1);
    s_last_job[sizeof s_last_job - 1] = '\0';
    s_printer_failed = 0;
    s_job_cancelled = 0;
    s_printing = 1;
    job_touch();
    led_grid_set_source(LG_SRC_AIRPRINT);
    line_source();
    printer_state_post(PS_EV_JOB_START);
    // The record's screen, with Apple's tone curve under it (stylewriter.c).
    char screen[sizeof s_set.dither];
    int gain = 0;
    ht_algo_t algo = HT_ATKINSON;
    if (!printers_port_screen(screen, sizeof screen, &gain) && ht_algo_from_name(screen, &algo))
        algo = HT_ATKINSON;
    job_pipe_target_t t = { 0 };
    s_job_pipe_ok = 0;
    if (stylewriter_job_begin(m, opts ? opts->quality : 0, algo, gain, &t)) return ESP_FAIL;
    esp_err_t err = job_pipe_begin_to(name, &t);
    s_job_pipe_ok = err == ESP_OK;
    return err;
}

// The rip is over and the printer task has sent its last frame, or the job was abandoned. On
// the pipeline task (or the IPP worker, when the pipeline never started): the last page is
// waited out and the printer told it is the end, or told to stop.
static void sw_finished(const job_pipe_stats_t *st, int ok)
{
    int cancelled = s_job_cancelled;
    int rc = stylewriter_job_end(!(st->rc == 0 && ok && !cancelled));
    int printed = rc == 0;
    s_printing = 0;
    job_touch();
    memdiag_mark("job done");           // W-1094
    s_last_pages = (size_t)st->pages;
    s_last_bytes = st->printer_bytes;
    s_sw_was_answering = 0;
    if (printed) {
        printers_port_answered(PR_HEARD_STYLEWRITER);   // its `?` named the record's model
        ESP_LOGI(TAG, "StyleWriter job: %d page%s, %u bytes in -> %u printer bytes in %ums, "
                 "all out in %ums", st->pages, st->pages == 1 ? "" : "s", (unsigned)st->doc_bytes,
                 (unsigned)st->printer_bytes, (unsigned)st->rip_ms, (unsigned)st->ms);
        ota_health_printed();           // W-1059: this firmware prints; under the job lock
    } else {
        ESP_LOGE(TAG, "StyleWriter job %s: %u bytes in, rc %d, printer %d%s",
                 cancelled ? "canceled" : "failed", (unsigned)st->doc_bytes, st->rc, rc,
                 ok ? "" : ", transfer truncated");
    }
    printer_state_post(printed ? PS_EV_JOB_DONE : cancelled ? PS_EV_JOB_CANCELLED
                                                            : PS_EV_JOB_FAILED);
    // A job that stopped short reset the printer, which takes 2 s to settle: the port task's
    // next `1` and `2` are due in 10 s, not now, or a printer still starting reads as gone.
    s_last_status_ms = uptime_ms();
    s_sw_owns = 0;
    s_ipp_owns = 0;
    line_source();
    xSemaphoreGive(s_job_lock);
    ipp_server_job_done_for(s_port_job_slot, printed);
    printers_apply();                   // W-1088: a change to the port's printer waited for it
}

// ------------------------------------------------------------- printers
// W-1088: the printer list (components/printers) keeps what is where; these
// are its board's side of the port printer.

static void port_live(pr_live_t *o)
{
    ps_view_t v = printer_state_view();
    int mine = s_line_src == LINE_IW;
    // W-1116: an ImageWriter is present and ready only once ESC ? has named its model since the
    // line came up, so the page never shows the wrong ImageWriter while the line is up but the
    // self-ID has not come back yet (about 2-3 s).
    o->present = o->ready = s_iw_ready == 1 && s_iw_named;
    o->printing = s_printing && !s_laser_owns;
    o->attention = mine && v.state == PS_ATTENTION;
    o->failed = mine && v.state == PS_FAILED;
    if (s_port_sw) {
        // W-1103: its own answers. A fault it reports is attention whether or not a job is
        // printing, and the page names it; present means it answered, or is the original
        // StyleWriter reading as ready while it is silent (sw_idle_silent), and ready that it
        // could print now.
        sw_state_t st = s_sw_state;
        o->present = s_port_answering || s_iw_ready == 1;
        o->ready = o->present && sw_state_ready(st);
        o->attention = s_port_answering && sw_state_fault(st);
        o->fault = o->attention ? (uint8_t)pr_fault_from_sw(st) : 0;
        o->cart = (uint8_t)s_sw_cart;
    }
}

// The page changed the port printer's halftone: the next job prints with it.
static void port_screen(const char *dither, int gain)
{
    settings_save_general(NULL, NULL, dither, gain);
    snprintf(s_set.dither, sizeof s_set.dither, "%s", dither);
    s_set.gain = (uint8_t)gain;
    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;
    job_pipe_set_screen(algo, s_set.gain);
    ESP_LOGI(TAG, "the port printer's screen now %s, gain %u%%", s_set.dither, s_set.gain);
}

// ------------------------------------------------------- a StyleWriter on the port
// W-1103: the port task (kick_task) asks it, a job reports through its session, and these
// turn what it says into the box's state, the panel's line and what the phone is told.

// The screen ImageWriter jobs and the test page print with follows the record on the port
// (W-1116): another ImageWriter record (the LQ for the II), or one changed back from a
// StyleWriter (no Apple screen), each with its own halftone and gain. Under the job lock: a
// job's rip keeps the screen it started with.
static void port_screen_follow(void)
{
    char d[sizeof s_set.dither];
    int g;
    if (!printers_port_screen(d, sizeof d, &g) && (strcmp(d, s_set.dither) || g != s_set.gain))
        port_screen(d, g);
}

// The port's record became a StyleWriter, or stopped being one: whose answers are the
// port's state changes hands. Only with no job on the port, which reads the family it began
// with (its panel, IPP state and cartridge): `locked` when the caller holds the job lock, else
// it is taken without waiting, and a flip that finds it held is the port task's to make once
// the job is over.
static void port_sw_sync(int sw, int locked)
{
    if (sw == s_port_sw) return;
    if (!locked && xSemaphoreTake(s_job_lock, 0) != pdTRUE) return;
    s_port_sw = sw;
    s_port_answering = 0;
    s_sw_cart = 0;
    s_sw_job_silent = 0;
    s_link_check = 1;                   // J2's link follows the record, under the job lock
    // The printer now on the record is asked what it has in, its ribbon or its cartridge, so
    // AirPrint offers colour for it and not for the last one (a look: ESC ? with the line up
    // for an ImageWriter, the plain question first for a StyleWriter).
    s_look = 1;
    if (sw) {
        s_iw_ready = -1;                // its answers will say; HSKo says nothing
        s_iw_named = 0;                 // an ImageWriter's name is not this record's
        s_sw_state = SWS_OFFLINE;
        s_last_plain_ms = port_plain_due(uptime_ms());
    } else {
        s_iw_ready = s_hsko;            // the line speaks again
        if (s_hsko >= 0) printer_state_port_line(s_hsko);
        port_screen_follow();
    }
    // The panel and the phone follow the record that is the port's now, in this call: the
    // caller may be the port task holding the job lock, which printers_changed's own sync
    // cannot take, so nothing else would post the line or push the slots until the next event.
    line_source();
    port_push(printer_state_view().state);
    if (!locked) xSemaphoreGive(s_job_lock);
}

// A cartridge was read: AirPrint offers colour or not under the same UUID, as the ribbon
// does, and it is kept so a restart offers what the printer last said. A black-only model
// never offers colour. Not for the printer task to call: this writes flash.
static void sw_cart_seen(int colour)
{
    const sw_spec_t *sp = sw_spec(printers_port_sw_model());
    if (!sp) return;                    // the record is no longer a StyleWriter: not its ribbon
    colour = colour > 0 && sp->colour;
    s_sw_cart = colour ? 2 : 1;
    iw_ribbon_t r = colour ? IW_RIBBON_COLOUR : IW_RIBBON_BLACK;
    if (r != s_ribbon) {
        ESP_LOGI(TAG, "cartridge now %s (was %s)", ribbon_name(r), ribbon_name(s_ribbon));
        s_ribbon = (uint8_t)r;
        settings_save_ribbon((uint8_t)r);
    }
    port_push(printer_state_view().state);      // the colour it offers (W-1116: its own slot)
}

// Whether a port record of `kind` and `model` may offer colour at all: a black-only
// StyleWriter never does, whatever the ImageWriter that was there before had in.
static int colour_offered(int kind, int model)
{
    if (kind != PR_KIND_STYLEWRITER) return 1;
    const sw_spec_t *sp = sw_spec(pr_model_to_sw((pr_model_t)model));
    return sp && sp->colour;
}

// What a saved ribbon may offer a phone at start, for printer 0 (the oldest port record,
// which is the port's printer until anything answers).
static int ribbon_offered(void)
{
    return colour_offered(s_port_sw ? PR_KIND_STYLEWRITER : PR_KIND_IMAGEWRITER,
                          pr_model_from_sw(printers_port_sw_model()));
}

// The session reports its state (sw_session.h) while it runs a job or a cleaning, on
// whichever task that is, the printer task included, so nothing here may write flash. The
// fault goes to the panel at once and to IPP in its own words, not only as a change of the
// display state.
static void sw_state_cb(sw_state_t st)
{
    s_sw_state = st;
    s_port_answering = st != SWS_OFFLINE;
    if (st == SWS_OFFLINE) {
        s_sw_job_silent = 1;            // the session got nothing: offline until it answers
        // It was answering before the session: the port is looked at once the session is over
        // (the plain question, then ESC ?), so an ImageWriter put in its place is found.
        if (s_sw_was_answering) {
            s_sw_was_answering = 0;
            s_look = 1;
        }
    }
    s_iw_ready = sw_state_ready(st);
    if (s_printing) {
        printer_state_post(sw_state_fault(st) ? PS_EV_FAULT_ON : PS_EV_FAULT_OFF);
        printer_state_post(st == SWS_OFFLINE ? PS_EV_LINE_NOT_READY : PS_EV_LINE_READY);
    }
    port_push(printer_state_view().state);
}

// What the port task's question (`1` and `2`, the plain question) found, idle: the state to
// show, and whether the record's printer answered. The original StyleWriter shows ready while it
// is silent and has not failed a session (sw_idle_silent), unanswering.
static void sw_idle_state(sw_state_t st, int answered)
{
    s_sw_state = st;
    s_port_answering = answered;
    if (answered) {
        s_sw_job_silent = 0;
        printers_port_answered(PR_HEARD_STYLEWRITER);
    }
    s_iw_ready = sw_state_ready(st);
    line_source();
    port_push(printer_state_view().state);
}

// The record's StyleWriter did not answer, idle.
static void sw_idle_quiet(void)
{
    sw_idle_state(sw_idle_silent(printers_port_sw_model(), s_sw_job_silent), 0);
}

// A StyleWriter that has just answered: its cartridge (what AirPrint offers follows it), and
// its state.
static void sw_online(sw_model_t m)
{
    int c = stylewriter_cart(m);
    if (c >= 0) sw_cart_seen(c);
    sw_state_t st;
    if (stylewriter_status(m, &st) == SW_OK) sw_idle_state(st, 1);
    else sw_idle_quiet();
    s_last_status_ms = uptime_ms();     // `1` and `2` were just asked: not again in 10 s
}

// The person named the port's printer a StyleWriter (Add a Printer…, the unknown printer's
// notice, the detail's Model): the full sequence, which finds the original too, confirms it
// and reads its model. A printer that stays silent keeps the pick, offline.
static void sw_confirm(void)
{
    int id = s_confirm_sw_id;
    // The record the person picked; skip if it is no longer a port StyleWriter (changed since).
    if (printers_test_kind(id) != 3) return;
    ESP_LOGI(TAG, "asking the port's StyleWriter what it is");
    sw_model_t m = stylewriter_ask_full();
    s_last_status_ms = uptime_ms();
    if (m == SW_NONE) {
        // The full sequence resets even the original: nothing answered. The picked record
        // keeps the pick, offline; the printer on the port (another record, or none) is left.
        ESP_LOGW(TAG, "the port's StyleWriter did not answer; keeping the pick, offline");
        s_sw_job_silent = 1;
        if (printers_port_is_stylewriter()) sw_idle_state(SWS_OFFLINE, 0);
        else port_push(printer_state_view().state);
        return;
    }
    // The picked record takes the model it named (when free), and that model is on the port now.
    printers_port_set_sw_model(id, m);
    port_sw_sync(printers_port_is_stylewriter(), 1);
    sw_online(m);
    ESP_LOGI(TAG, "port task: %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static int sw_cancelled(void)
{
    if (s_sw_owns) return s_job_cancelled;
    return s_test_running && s_test_cancel;      // the test page's second tap
}

// W-1116: the port last answered as an ImageWriter, so the StyleWriter's language must not
// go out (stylewriter_cfg_t.swapped), whichever record a job, page or cleaning is for.
static int sw_swapped(void)
{
    return printers_port_other_family(PR_HEARD_STYLEWRITER);
}

// The settings page's Print Head cleaning (printers_cfg_t.clean): one cleaning, on a task
// of its own so the page is answered. The job lock is held to its end.
static void sw_clean_task(void *arg)
{
    sw_model_t m = (sw_model_t)(intptr_t)arg;
    int rc = stylewriter_clean(m);
    s_sw_was_answering = 0;
    if (rc == SW_OK) printers_port_answered(PR_HEARD_STYLEWRITER);
    printer_state_post(rc == SW_OK ? PS_EV_JOB_DONE : PS_EV_JOB_FAILED);
    s_last_status_ms = uptime_ms();
    s_printing = 0;
    job_touch();
    line_source();
    ESP_LOGI(TAG, "cleaning task: %u bytes of stack to spare", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

// 0 once it has begun, 409 when a job holds the printer, 502 when it is not the port's
// StyleWriter, has no cleaning (the original), does not answer or holds a fault (a cleaning
// would wait on it with the lock held): stylewriter_clean_check decides, with the lock.
static int sw_clean_web(int id)
{
    if (printers_test_kind(id) != 3) return 502;
    int busy = xSemaphoreTake(s_job_lock, 0) != pdTRUE;
    if (!busy) {                                 // a new session: what it finds is the state
        s_sw_job_silent = 0;
        s_sw_was_answering = s_port_answering;
    }
    sw_model_t m = printers_port_sw_model();
    int rc = stylewriter_clean_check(m, busy);
    if (rc) {
        if (!busy) xSemaphoreGive(s_job_lock);
        return rc;
    }
    printer_state_post(PS_EV_ACTIVITY);          // a person asked: wake the panel
    s_printing = 1;
    job_touch();
    led_grid_set_source(LG_SRC_AIRPRINT);
    line_source();
    printer_state_post(PS_EV_JOB_START);
    if (xTaskCreate(sw_clean_task, "sw_clean", 4096, (void *)(intptr_t)m, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cleaning: could not create task");
        printer_state_post(PS_EV_JOB_FAILED);
        s_printing = 0;
        job_touch();
        line_source();
        xSemaphoreGive(s_job_lock);
        return 502;
    }
    return 0;
}

// On the HTTP task: the full sequence takes 2 to 12 s, so it is the port task's to run. The
// record the person picked (W-1116) is confirmed, not the newest StyleWriter.
static void sw_confirm_queue(int id)
{
    s_confirm_sw_id = id;
    s_confirm_sw = 1;
}

// "Look for printers": the port is asked again (the ImageWriter's self-ID once the line is
// ready, the StyleWriter's plain question while nothing answers), and AppleTalk is looked on
// every 30 s again for a while.
static void printers_look(void)
{
    s_look_person = 1;
    s_look = 1;
    laser_look_now();
}

static void offers_apply(void);

// W-1116: the person set a port record's model (Add a Printer…, the Model detail): the port's
// last answer is forgotten for the record on the port (R10), so the port is asked again at once
// to find what is there. The policy gives the look the right question: the plain one first on a
// StyleWriter-first port, and nothing on a Something else port but the person's own look.
static void port_model_changed(int id)
{
    (void)id;
    s_look = 1;
}

static void printers_changed(void)
{
    port_sw_sync(printers_port_is_stylewriter(), 0);
    // The record on the port may be another of the same family now (the LQ for the II): the
    // screen follows it under the job lock; a job in progress keeps its own, and the port task
    // follows once the job is over (s_screen_check).
    if (!s_port_sw) {
        if (xSemaphoreTake(s_job_lock, 0) == pdTRUE) {
            port_screen_follow();
            xSemaphoreGive(s_job_lock);
        } else {
            s_screen_check = 1;
        }
    }
    line_source();
    offers_apply();
    // What the phone is told about each port record follows the list (W-1116): its slot,
    // which record is the printer on the port, and the colour it may offer (a black-only
    // StyleWriter, the record changed to one, offers none).
    port_push(printer_state_view().state);
}

static void printers_web_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    printers_json(buf, cap);
}

static int printers_web_post(void *ctx, const char *body, char *err, size_t cap)
{
    (void)ctx;
    return printers_post(body, err, cap);
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
    // A Mac's stream is the ImageWriter driver's: it never goes to a StyleWriter (W-1103), nor
    // to an ImageWriter record's port where a StyleWriter answered last (the Mac hears busy
    // until the person says). The offers keep Macs from asking; this keeps a stray one from
    // being heard.
    port_sw_sync(printers_port_is_stylewriter(), 1);
    if (s_port_sw || printers_port_conflict()) {
        xSemaphoreGive(s_job_lock);
        return 1;
    }
    stylewriter_link(0);
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
    line_source();                             // the port's own line, not the box's
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
    if (paper_to_tof() != ESP_OK) goto fail;
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
    // The Mac's driver feeds to the next page by line, never by form feed.
    iw_link_job_end_at(&s_link, flushed == ESP_OK, iw_scan_at_tof(&s_lt_scan));
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
    line_source();
    // W-1059, under the job lock; an empty job (a Mac that opened and closed,
    // or an idle relay peer) proves nothing about this firmware.
    if (flushed == ESP_OK && s_lt_bytes) ota_health_printed();
    xSemaphoreGive(s_job_lock);
    return flushed == ESP_OK;
}

// ------------------------------------------------------------- dip / test
// DIP1-8 select mode/dev-mode/test-on-boot/dither-override/spare; a press of
// the test button prints a self-test page. dev_mode and dither_ovr land in
// later milestones -- for this rev dev_mode is a log note only (each
// printer's uuid is its record's since W-1088).

// The port task. It runs every 500 ms and asks the printer on J2 whatever the policy
// (port_policy.h, W-1103) says the port is due, under the job lock, taken without waiting,
// which keeps it clear of any job's writes or a test page. The policy is pure and
// host-tested; what it picks is done here:
//   PA_ESCQ    the ImageWriter's self-ID (W-1064): the ribbon, and what is on the port
//   PA_PLAIN   the StyleWriter's plain question, one `?` at 57600
//   PA_STATUS  a StyleWriter that answers: `1` and `2` every 10 s
//   PA_KICK    W-1039: unstick an ImageWriter that serial-line noise (a cable replug,
//              power-up) left busy with SELECT lit -- CTRL-X cancels the unprinted line in
//              its buffer. printer_state decides when (idle, busy 5 s, then every 60 s at
//              most), and only an ImageWriter, or a port nothing has been found on yet (one
//              left busy at power-up never answers ESC ?), is ever kicked (W-1088).
static void kick_task(void *arg)
{
    (void)arg;
    static const uint8_t ctrl_x = 0x18;     // ImageWriter II: cancel current line
    int boot_looked = 0, again = 0;
    uint32_t drops = s_line_drops;
    s_last_plain_ms = uptime_ms();           // the first cadence question is 5 s from now
    for (;;) {
        if (!again) vTaskDelay(pdMS_TO_TICKS(500));
        again = 0;
        port_mode_t mode = printers_port_mode();
        port_sw_sync(mode == PORT_STYLEWRITER, 0);
        if (s_port_sw != (mode == PORT_STYLEWRITER)) continue;   // a job has the port: after it

        // J2's link follows the port's record (the kind may have changed), and a StyleWriter the
        // person named is asked what it is. Both need the printer to themselves.
        if ((s_link_check || s_confirm_sw || s_screen_check) && xSemaphoreTake(s_job_lock, 0) == pdTRUE) {
            if (s_link_check) {
                s_link_check = 0;
                stylewriter_link_record();
            }
            if (s_screen_check) {
                s_screen_check = 0;
                port_screen_follow();
            }
            if (s_confirm_sw) {
                s_confirm_sw = 0;
                sw_confirm();
            }
            xSemaphoreGive(s_job_lock);
            continue;
        }

        uint32_t now = uptime_ms();
        int line_ready = s_hsko == 1;
        // What the list holds (W-1116) sets the order of the questions, read in one pass; the
        // current port record's kind (mode) is the policy's.
        int sw_listed, iw1_listed, sw1_listed;
        printers_port_listed(&sw_listed, &iw1_listed, &sw1_listed);
        // Boot: after the quiet power-on, a port with no line up is asked the plain question
        // (a StyleWriter may never raise it). A StyleWriter the board already knows is asked
        // at once, the original with the full sequence: it answers nothing else until reset,
        // and only an answer tells it from an empty port.
        if (!boot_looked && (mode == PORT_STYLEWRITER || printer_state_view().state != PS_BOOTING)) {
            boot_looked = 1;
            // An original StyleWriter record anywhere in the list is owed the full sequence at
            // start-up (W-1116: keyed on the list, not the oldest record); a StyleWriter on the
            // port is primed for the plain question; an empty or ImageWriter port with the line
            // down is looked at.
            if (sw1_listed) s_full_due = 1;
            if (mode == PORT_STYLEWRITER) s_last_plain_ms = port_plain_due(now);
            else if (!line_ready) s_look = 1;
        }
        // The line came up after a second or more down (or was first seen), 2 s ago (W-1064).
        // A line-up edge reaches the policy even while a StyleWriter answers (R11): the status
        // its edge asks at once finds an ImageWriter put in its place in about 1.5 s.
        int line_up = s_ribbon_want && (int32_t)(xTaskGetTickCount() - s_ribbon_due) >= 0;
        // Something else: the board's own look asks nothing of AirWriter's own (the policy gates
        // it), but the person's Look for Printers asks once.
        if (mode == PORT_UNKNOWN_SAID && !s_look_person) s_look = 0;
        int look = s_look, look_person = s_look_person;

        port_in_t in = {
            .kind = mode == PORT_NONE ? -1 : mode == PORT_IMAGEWRITER ? PR_KIND_IMAGEWRITER
                  : mode == PORT_STYLEWRITER ? PR_KIND_STYLEWRITER : PR_KIND_UNKNOWN,
            .iw1_listed = iw1_listed, .sw_listed = sw_listed, .sw1_listed = sw1_listed,
            .said_other = mode == PORT_UNKNOWN_SAID, .line_ready = line_ready,
            .want_kick = (mode == PORT_IMAGEWRITER || mode == PORT_NONE) && printer_state_want_kick(),
            .heard = printers_port_heard(), .now_ms = now,
            .line_up_edge = line_up, .look = look, .look_person = look && look_person,
            .answering = s_port_answering, .last_plain_ms = s_last_plain_ms,
            .last_status_ms = s_last_status_ms, .plain_asked = s_plain_asked,
            .plain_unanswered = s_plain_unanswered, .event_plain = s_event_plain,
            .full_due = s_full_due, .confirm = s_port_confirm,
        };
        if (s_line_drops != drops) {         // a new busy spell gets a fresh plain question
            drops = s_line_drops;
            port_line_down(&in);
            s_plain_asked = in.plain_asked;
            s_plain_unanswered = in.plain_unanswered;
            in.confirm = s_port_confirm = PA_NONE;   // the printer sighted once is gone
        }
        port_tick(&in);
        s_port_answering = in.answering;
        port_act_t act = port_policy(&in);
        if (act == PA_NONE) continue;
        if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
            s_port_confirm = PA_NONE;        // a job has the port: the sighting is stale by its end
            continue;
        }

        // What the question got, for port_after: the family that answered and whether it was the
        // record's own printer. Its books are written back before anything that follows up an
        // answer, which sets answering from what it then finds.
        int heard = PR_HEARD_NONE, mine = 0;
        // The seen call's result (printers_port_seen / _sw): the record id the answer belongs
        // to, PR_SEEN_AGAIN for a first sighting of a model with no record, or -1 for no seen
        // call. `mine` is whether it named a record; a first sighting is repeated (`confirm`).
        int seen = -1;
        sw_model_t plain = SW_NONE;             // the model the plain question or the full sequence named
        switch (act) {
        case PA_ESCQ: {
            int answered = ribbon_check(0);     // printers_port_seen: the record, or the notice
            seen = s_seen_again;                // ribbon_ask kept its result
            // The record on the port may be another family's now (an ImageWriter put in a
            // StyleWriter's place): the flip is made here, under the lock, so the panel and the
            // phone go Ready in this iteration (printers_changed's own sync cannot take the
            // job lock the port task holds).
            port_sw_sync(printers_port_is_stylewriter(), 1);
            heard = answered ? PR_HEARD_IMAGEWRITER : PR_HEARD_NONE;
            mine = seen > 0;
            again = !answered;                  // the plain question may follow at once
            break;
        }
        case PA_PLAIN:
        case PA_FULL:
            // The full sequence only after the person's look found nothing on an original
            // StyleWriter record's port (port_policy.h): the original answers once reset.
            plain = act == PA_FULL ? stylewriter_ask_full() : stylewriter_ask_plain();
            if (plain != SW_NONE) {
                seen = printers_port_seen_sw(plain);   // makes/finds the record, or PR_SEEN_AGAIN
                port_sw_sync(printers_port_is_stylewriter(), 1);
                heard = PR_HEARD_STYLEWRITER;
                mine = seen > 0;
            }
            break;
        case PA_STATUS:
            break;                              // asked below, with its state
        case PA_KICK: {
            stylewriter_link(0);             // CTRL-X is the ImageWriter's: at its link, as ESC ? is
            esp_err_t err = serial_out_send_unflowed(&ctrl_x, 1);
            stylewriter_link_record();
            printer_state_kicked();
            ESP_LOGI(TAG, "printer busy while idle; sent CTRL-X (%s)", esp_err_to_name(err));
            break;
        }
        case PA_NONE:
            break;
        }

        sw_model_t m = printers_port_sw_model();
        sw_state_t st = SWS_OFFLINE;
        int status_rc = SW_E_SILENT, sw_swap = 0;
        if (act == PA_STATUS && m != SW_NONE) {   // (SW_NONE: the record changed under us)
            status_rc = stylewriter_status(m, &st);
            if (status_rc == SW_OK) {
                heard = PR_HEARD_STYLEWRITER;
                mine = 1;
            }
            // Also ask who is there (the plain question asks `?`, and `p` after `CS` as a look
            // does): a different StyleWriter put in its place shows within one check (W-1116).
            sw_model_t m2 = stylewriter_ask_plain();
            if (m2 != SW_NONE && m2 != m) {
                seen = printers_port_seen_sw(m2);      // makes/finds its record, or PR_SEEN_AGAIN
                heard = PR_HEARD_STYLEWRITER;
                mine = seen > 0;
                sw_swap = seen > 0;
                // Its record is current: the flip, if any, here under the lock. A first
                // sighting changes nothing yet (the plain question repeats it, below), so the
                // mode stays and the status just read is not written into another record.
                if (sw_swap) port_sw_sync(printers_port_is_stylewriter(), 1);
            }
            // One that answered `?` but not `1` and `2` reads as its status says, not
            // answering: the stop's look (the plain question, then ESC ?) finds it again.
        }
        in.now_ms = uptime_ms();
        port_after(&in, act, heard, mine);
        if (in.look != look) s_look = in.look;        // answered, or a StyleWriter's stop to look at
        if (look_person && !in.look_person) s_look_person = 0;
        // ESC ? and the status end a line-up not yet due as well, as they always did.
        if ((line_up && !in.line_up_edge) || act == PA_ESCQ || act == PA_STATUS) s_ribbon_want = 0;
        s_port_answering = in.answering;
        s_last_plain_ms = in.last_plain_ms;
        s_last_status_ms = in.last_status_ms;
        s_plain_asked = in.plain_asked;
        s_plain_unanswered = in.plain_unanswered;
        s_event_plain = in.event_plain;
        s_full_due = in.full_due;
        // A first sighting of a model with no record (PR_SEEN_AGAIN): the same question again
        // at once, and the same answer twice makes the record (port_policy.h's `confirm`).
        // A status check's sighting is confirmed by the plain question, which names the model
        // whatever record is current (the status needs a StyleWriter record's model).
        s_port_confirm = seen != PR_SEEN_AGAIN ? PA_NONE : act == PA_STATUS ? PA_PLAIN : act;

        if ((act == PA_PLAIN || act == PA_FULL) && mine) {
            sw_online(plain);
            s_look = 0;                       // its cartridge and state were just read
            // The deepest the port task goes: the plain question, the record, the cartridge
            // (which saves the ribbon) and the status.
            ESP_LOGI(TAG, "port task: %u bytes of stack to spare",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        } else if (act == PA_PLAIN && s_port_sw) {
            // A StyleWriter record whose printer did not answer as it: another StyleWriter there
            // is not the record's (the page and the phone show a swap), and silence is offline
            // (but the original's, sw_idle_silent).
            if (plain != SW_NONE) sw_idle_state(SWS_OFFLINE, 0);
            else sw_idle_quiet();
        } else if (act == PA_FULL && s_port_sw) {
            // Start-up or the person's look, and not even the full sequence got the record's
            // printer: it is offline (another StyleWriter there shows as a swap).
            if (plain == SW_NONE) s_sw_job_silent = 1;
            sw_idle_state(SWS_OFFLINE, 0);
        } else if (act == PA_STATUS && m != SW_NONE) {
            // A different StyleWriter answered the plain question: it is current now; read its
            // cartridge and state fresh. Else the current record's own status.
            if (sw_swap) sw_online(printers_port_sw_model());
            else if (seen != PR_SEEN_AGAIN) {       // (a first sighting of another: not this record's status)
                if (status_rc == SW_OK) sw_idle_state(st, 1);
                else sw_idle_quiet();
            }
        } else if (act == PA_ESCQ && mine) {
            // The ImageWriter on the port is its record's: the flip's own look is answered, and
            // the screen follows the record while the lock is held.
            s_look = 0;
            port_screen_follow();
            port_push(printer_state_view().state);
        } else if (act == PA_ESCQ || act == PA_PLAIN) {
            port_push(printer_state_view().state);   // the port's family may be another now
        }
        xSemaphoreGive(s_job_lock);
    }
}

// A person did something; wake the panel. From the IPP, DIP and test-button paths.
static void on_activity(void)
{
    printer_state_post(PS_EV_ACTIVITY);
}

// The page's stop: a second tap. Asked before every line and strip, so it
// also notes how low the internal heap goes while the page holds its bands.
static size_t s_tp_heap_low;
static size_t s_tp_bytes;

static int test_page_stop(void *ctx)
{
    (void)ctx;
    size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (f < s_tp_heap_low) s_tp_heap_low = f;
    // A failed write (a cancel, a stall) drops the rest; no point making it.
    return s_test_cancel || s_printer_failed;
}

static void test_page_out(void *ctx, const uint8_t *buf, size_t n)
{
    s_tp_bytes += n;
    printer_out(ctx, buf, n);
}

// What the page says AppleTalk is: the ways in that are on, as the settings
// page's cards name them.
static void appletalk_text(char *out, size_t cap)
{
    int tunnel = 0, radio = 0, infinite = 0;
    char zone[40];
    if (ethertalk_running()) ethertalk_settings(&tunnel, &radio, &infinite, zone, sizeof zone);
    const char *on[4];
    int n = 0;
    if (localtalk_serving()) on[n++] = "AppleTalk port";
    if (radio)    on[n++] = "networked Macs";
    if (tunnel)   on[n++] = "Basilisk II";
    if (infinite) on[n++] = "Infinite Mac";
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        snprintf(out + strlen(out), cap - strlen(out), "%s%s", i ? ", " : "", on[i]);
    if (!n) snprintf(out, cap, "Off");
}

static void test_page_task(void *arg)
{
    (void)arg;
    stylewriter_link(0);            // the ImageWriter's page goes at its link, from the first byte
    s_stream_crc = 0;
    s_printer_failed = 0;
    iw_link_job_start(&s_link);
    led_grid_set_source(LG_SRC_AIRPRINT);
    line_source();                  // the port's own line, not the box's
    printer_state_post(PS_EV_JOB_START);
    // As a job does: the page prints for the ribbon that is in now. The wait
    // for a printer that is not ready ends on a second tap; an AirPrint
    // job's cancel, left set by the last one, must not end it.
    s_job_cancelled = 0;
    ribbon_check(1);

    ht_algo_t algo;
    if (ht_algo_from_name(s_set.dither, &algo)) algo = HT_ATKINSON;
    const esp_app_desc_t *app = esp_app_get_description();
    char built[sizeof app->date], ap[32], at[80];
    size_t k = 0;                               // "Oct  5 2026" reads "Oct 5 2026"
    for (const char *c = app->date; *c && k + 1 < sizeof built; c++)
        if (!(*c == ' ' && k && built[k - 1] == ' ')) built[k++] = *c;
    built[k] = '\0';
    snprintf(ap, sizeof ap, "AirWriter-%s", s_suffix);
    appletalk_text(at, sizeof at);
    char name[PR_NAME_MAX];
    printers_port_name(name, sizeof name);
    tp_info_t info = {
        .version = app->version, .built = built, .model = pr_model_name(printers_port_model()),
        .name = name,
        .host = s_set.hostname, .ip = s_ip, .setup_ssid = s_ap_mode ? ap : NULL,
        .appletalk = at, .ribbon = (iw_ribbon_t)s_ribbon, .screen = algo, .gain = s_set.gain,
        .top_margin = s_set.top_margin,
    };
    tp_io_t io = { .out = test_page_out, .stop = test_page_stop };
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_tp_heap_low = heap0;
    s_tp_bytes = 0;
    tp_result_t rc = tp_print(&info, &io);
    if (rc == TP_NOMEM) s_printer_failed = 1;
    ESP_LOGI(TAG, "test page: %s, %u bytes, crc32 %08x; internal heap %u KB free, "
                  "%u KB at its lowest; %u bytes of stack to spare",
             rc == TP_DONE ? "sent" : rc == TP_STOPPED ? "stopped" : "out of memory",
             (unsigned)s_tp_bytes, (unsigned)s_stream_crc, (unsigned)(heap0 / 1024),
             (unsigned)(s_tp_heap_low / 1024), (unsigned)uxTaskGetStackHighWaterMark(NULL));

    // As job_finished does: the last byte has to clear before the page counts
    // as printed, a cancel stops that wait, and a tail that did not go out is
    // purged so it cannot print ahead of the next job, which then resyncs.
    esp_err_t flushed = s_printer_failed ? ESP_FAIL
                                         : serial_out_flush(CONFIG_AIRWRITER_STALL_ABORT * 1000u);
    if (flushed != ESP_OK) serial_out_purge();
    iw_link_job_end(&s_link, flushed == ESP_OK);

    // The cancel is cleared with the page: ribbon_check's wait reads it for
    // every job, and a stale one would skip asking the ribbon.
    portENTER_CRITICAL(&s_tap_mux);
    s_test_running = 0;
    int cancelled = s_test_cancel;
    s_test_cancel = 0;
    portEXIT_CRITICAL(&s_tap_mux);
    // A cancel that came too late to cut anything is a page that printed.
    printer_state_post(flushed == ESP_OK ? PS_EV_JOB_DONE
                       : cancelled ? PS_EV_JOB_CANCELLED : PS_EV_JOB_FAILED);
    stylewriter_link_record();
    s_printing = 0;
    job_touch();
    line_source();
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
    if (xTaskCreate(test_page_task, "test_page", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "test page: could not create task");
        portENTER_CRITICAL(&s_tap_mux);
        s_test_running = 0;
        portEXIT_CRITICAL(&s_tap_mux);
        s_printing = 0;
        job_touch();
        xSemaphoreGive(s_job_lock);
    }
}

// W-1088: a LaserWriter's test page (ps_testpage), sent as it is on its own
// task: the printer takes its time. s_job_lock is held; the task gives it back.
static volatile int s_ps_test_running;

static void ps_test_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    char name[PR_NAME_MAX] = "", product[64] = "", version[16] = "", board[96], host[64];
    printers_describe(id, name, sizeof name, product, sizeof product, version, sizeof version);
    snprintf(board, sizeof board, "AirWriter %s at %s", esp_app_get_description()->version, s_ip);
    snprintf(host, sizeof host, "http://%s.local/", s_set.hostname);
    ps_tp_info_t ti = { .printer = name, .product = product, .version = version, .host = host,
                        .board = board };
    int printed = laser_test_page(id, &ti);
    ESP_LOGI(TAG, "test page to \"%s\": %s", name, printed ? "printed" : "not printed");
    printer_state_post(printed ? PS_EV_JOB_DONE : PS_EV_JOB_FAILED);
    s_ps_test_running = 0;
    s_printing = 0;
    s_laser_owns = 0;
    job_touch();
    line_source();
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

static void ps_test_locked(int id)
{
    strncpy(s_last_job, "Test page", sizeof s_last_job - 1);
    s_printing = 1;
    s_laser_owns = 1;
    s_ps_test_running = 1;
    job_touch();
    led_grid_set_source(LG_SRC_AIRPRINT);
    line_source();
    printer_state_post(PS_EV_JOB_START);
    if (xTaskCreate(ps_test_task, "ps_test", 6144, (void *)(intptr_t)id, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "test page: could not create task");
        printer_state_post(PS_EV_JOB_FAILED);
        s_ps_test_running = 0;
        s_printing = 0;
        s_laser_owns = 0;
        job_touch();
        line_source();
        xSemaphoreGive(s_job_lock);
    }
}

// W-1103: a StyleWriter's test page is a raster drawn on the board (sw_testpage.h) and fed to
// a session as it is made (stylewriter_test_page). It never hears the ImageWriter's page, which
// is ImageWriter commands. Its own task, as the ImageWriter's page has, so that the button's
// poll task and the settings page are answered at once; the task's stack is internal, as the
// cartridge it reads may be saved to flash. The second tap of the button is the cancel
// (sw_cancelled), which the session reads between steps: a block in flight finishes.
static void sw_test_task(void *arg)
{
    sw_model_t m = (sw_model_t)(intptr_t)arg;
    char screen[sizeof s_set.dither], name[PR_NAME_MAX], board[112], host[64];
    int gain = 0;
    ht_algo_t algo = HT_ATKINSON;
    if (!printers_port_screen(screen, sizeof screen, &gain) && ht_algo_from_name(screen, &algo))
        algo = HT_ATKINSON;
    printers_port_name(name, sizeof name);
    int has_ip = s_ip[0] && strcmp(s_ip, "0.0.0.0");
    if (has_ip) snprintf(board, sizeof board, "AirWriter %s at %s", esp_app_get_description()->version, s_ip);
    else        snprintf(board, sizeof board, "AirWriter %s, no IP yet", esp_app_get_description()->version);
    // The settings page's address: its host name, or in setup mode the board's own.
    if (s_ap_mode) snprintf(host, sizeof host, "%s", s_ip);
    else           snprintf(host, sizeof host, "%s.local", s_set.hostname);
    sw_tp_info_t info = { .printer = name, .board = board, .host = host, .model = m,
                          .colour_cart = s_sw_cart == 2, .gain = gain, .screen = algo };
    int rc = stylewriter_test_page(m, &info);
    s_sw_was_answering = 0;
    if (rc == SW_OK) printers_port_answered(PR_HEARD_STYLEWRITER);
    ESP_LOGI(TAG, "StyleWriter test page: %s (%d); %u bytes of stack to spare",
             rc == SW_OK ? "printed" : rc == SW_E_CANCEL ? "cancelled" : "not printed", rc,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    // As the ImageWriter's page ends: the cancel is cleared with the page under the tap's own
    // lock, so a tap that lands as it finishes cannot cancel the next job.
    portENTER_CRITICAL(&s_tap_mux);
    s_test_running = 0;
    s_sw_test_running = 0;
    int cancelled = s_test_cancel;
    s_test_cancel = 0;
    portEXIT_CRITICAL(&s_tap_mux);
    printer_state_post(rc == SW_OK ? PS_EV_JOB_DONE : cancelled ? PS_EV_JOB_CANCELLED : PS_EV_JOB_FAILED);
    // The session spoke to the printer and may have reset it: `1` and `2` are due in 10 s, not now.
    s_last_status_ms = uptime_ms();
    s_printing = 0;
    job_touch();
    line_source();
    xSemaphoreGive(s_job_lock);
    vTaskDelete(NULL);
}

// s_job_lock is held; the page's task gives it back.
static void sw_test_locked(void)
{
    s_sw_job_silent = 0;                // a new session: what it finds is the state
    s_sw_was_answering = s_port_answering;
    strncpy(s_last_job, "Test page", sizeof s_last_job - 1);
    s_printing = 1;
    job_touch();
    led_grid_set_source(LG_SRC_AIRPRINT);
    line_source();
    printer_state_post(PS_EV_JOB_START);
    portENTER_CRITICAL(&s_tap_mux);
    s_test_running = 1;
    s_sw_test_running = 1;
    s_test_cancel = 0;
    portEXIT_CRITICAL(&s_tap_mux);
    if (xTaskCreate(sw_test_task, "sw_test", 6144, (void *)(intptr_t)printers_port_sw_model(), 3,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "test page: could not create task");
        portENTER_CRITICAL(&s_tap_mux);
        s_test_running = 0;
        s_sw_test_running = 0;
        portEXIT_CRITICAL(&s_tap_mux);
        printer_state_post(PS_EV_JOB_FAILED);
        s_printing = 0;
        job_touch();
        line_source();
        xSemaphoreGive(s_job_lock);
    }
}

// The test page for whichever printer gets it (printers_test_target): the
// port's ImageWriter's, a LaserWriter's, or the port's StyleWriter's. s_job_lock is held.
static void test_page_to(int id, int port)
{
    port_sw_sync(printers_port_is_stylewriter(), 1);
    if (printers_test_kind(id) == 3) sw_test_locked();
    else if (port) test_page_locked();
    else ps_test_locked(id);
}

// W-1103: the port's printer's test page, while the other family than its record's answered
// on the port last, would be in the wrong language: it is not printed.
static int port_page_refused(int port)
{
    if (!port || !printers_port_conflict()) return 0;
    ESP_LOGW(TAG, "test page refused: another printer answered on the printer port");
    return 1;
}

// DIP4 at boot. Non-blocking: a test page never queues behind whatever is
// printing; s_job_lock is the actual mutual exclusion.
static void test_page(void)
{
    on_activity();
    int port, id = printers_test_target(&port);
    if (id < 0) {
        ESP_LOGW(TAG, "test page: no printer to print it on");
        return;
    }
    if (port_page_refused(port)) return;
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "test page requested but the printer is already busy");
        s_job_waiting = 1;
        return;
    }
    test_page_to(id, port);
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
        if (s_ps_test_running) {                   // W-1088: a LaserWriter's page
            ESP_LOGI(TAG, "test button tapped again: cancelling the test page");
            laser_job_cancel();
            return;
        }
        portENTER_CRITICAL(&s_tap_mux);
        if (s_test_running) {
            // An ImageWriter's write in progress is aborted; W-1041's resync runs before
            // the next job. A StyleWriter's session stops at its next step instead (its
            // writes are in whole blocks and never cut), and leaves the abort flag alone.
            s_test_cancel = 1;
            if (!s_sw_test_running) serial_out_set_abort(1);
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
    // W-1088: the port's ImageWriter, else the first shared printer.
    int port, id = printers_test_target(&port);
    // W-1103: another printer than the record's answered on the port: no test page for it, and
    // its low line is no sign of a cable either.
    if (port_page_refused(port)) {
        xSemaphoreGive(s_job_lock);
        led_grid_blip(LG_BLIP_BUSY);
        return;
    }
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
    // A StyleWriter's HSKo says nothing, so a low line is no sign of a cable (W-1103).
    if (((port && !printers_port_is_stylewriter()) || id < 0) && !serial_out_clear()) {
        ESP_LOGI(TAG, "test button with the printer not ready: testing the cable");
        if (xTaskCreate(cable_button_task, "cable", 4096, NULL, 3, NULL) != pdPASS)
            xSemaphoreGive(s_job_lock);
        return;
    }
#endif
    if (id < 0) {
        ESP_LOGW(TAG, "test button: no printer to print the test page on");
        xSemaphoreGive(s_job_lock);
        led_grid_blip(LG_BLIP_BUSY);
        return;
    }
    test_page_to(id, port);
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
        ESP_LOGI(TAG, "dip: dev_mode is on; it changes nothing this rev");
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
    // The settings page's Erase: its reply goes out first, as Restart's does.
    // Without it the page waited 30 s and said AirWriter had not answered.
    if (arg) vTaskDelay(pdMS_TO_TICKS(300));
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
    if (xTaskCreate(reset_task, "reset", 3072, (void *)1, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "factory reset: could not start");
        led_grid_action(LG_ACT_RESET_FAILED, 0);
    }
}

static int web_test_page(void *ctx, int which)
{
    (void)ctx;
    on_activity();
    int port, id = printers_test_target(&port);
    if (which >= 0) {                              // W-1088: the page names its printer
        int k = printers_test_kind(which);
        if (!k) return 2;
        id = which;
        port = k != 2;                             // the port's ImageWriter or StyleWriter
    }
    if (id < 0) return 2;
    if (port_page_refused(port)) return 3;
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) return 1;
    test_page_to(id, port);
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
    case PS_FAILED:    return "The last job failed";
    case PS_PRINTING:  return "Printing";
    case PS_OFFLINE:   return "Offline";
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
    char e_ssid[80], e_queue[80], e_job[200], e_model[80], e_name[80], e_host[96], e_phost[96], e_uri[200];
    // W-1103/W-1116: the printer on the port, as AirPrint names it, whichever family and
    // model (an ImageWriter by its record's model; the II with no ImageWriter record, as it
    // has always been reported).
    const sw_spec_t *swp = s_port_sw ? sw_spec(printers_port_sw_model()) : NULL;
    const char *model = swp ? swp->make_model
                      : printer_caps_imagewriter_model(printers_port_model())->make_model;

    char laser[300], printers[128];
    laser_status_json(laser, sizeof laser);
    printers_status_json(printers, sizeof printers);

    // W-1066: where the AirPrint job in the pipeline (or the last one) has got.
    job_pipe_stats_t pst;
    size_t queued;
    job_pipe_peek(&pst, &queued);

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
        "\"ssid\":\"%s\",\"dither\":\"%s\",\"gain\":%u,\"top_margin\":%d,\"signal\":\"%s\","
        "\"channel\":\"%s\",\"security\":\"%s\",\"uri\":\"%s\","
        "\"uptime\":\"%uh %02um\",\"reset\":\"%s\",\"version\":\"%s\",\"heap\":\"%u KB\","
        "\"slot\":\"%s\",\"trial\":%s,\"rolled_back\":\"%s\","
        "\"known_good\":\"%s\",\"known_good_version\":\"%s\","
        "\"model\":\"%s\",\"pic\":%s,"
        "\"pmodel\":\"%s\",\"pname\":\"%s\",\"phost\":\"%s\",\"airprint\":%s,"
        "\"lt_port\":%d,\"lt\":%s,\"lt_chip\":\"%s\",\"dip\":\"%s\",\"ap\":\"AirWriter-%s\","
        "\"laser\":%s,\"printers\":%s,\"fresh\":%s,\"internal\":{\"free\":%u,\"lowest\":%u},"
        "\"pipe\":{\"in\":%u,\"out\":%u,\"queued\":%u,\"upload_ms\":%u,\"rip_ms\":%u,\"ms\":%u}}",
        state_text(v.state),
        ps_name(v.state),
        v.asleep ? "true" : "false",
        attention || offline ? "true" : "false",
        s_ap_mode ? "true" : "false",
        json_esc(e_job, sizeof e_job, job),
        attention || failed ? "true" : "false",
        (s_port_sw ? s_iw_ready == 1 : serial_out_clear()) ? "true" : "false",
        s_printing ? "true" : "false", ribbon_name(s_ribbon),
        json_esc(e_queue, sizeof e_queue, s_set.queue), json_esc(e_host, sizeof e_host, s_set.hostname), s_ip,
        // in setup mode, the network it could not join ("" if none was set)
        json_esc(e_ssid, sizeof e_ssid, joined ? (char *)ap.ssid : s_set.ssid),
        s_set.dither, (unsigned)s_set.gain, (int)s_set.top_margin,
        joined ? (ap.rssi > -55 ? "Excellent" : ap.rssi > -67 ? "Good"
                : ap.rssi > -75 ? "Fair" : "Weak") : "",
        joined ? "2.4 GHz" : "",
        joined ? auth_name(ap.authmode) : "",
        s_ap_mode ? "" : json_esc(e_uri, sizeof e_uri, ipp_server_uri()),
        (unsigned)(up / 3600), (unsigned)((up / 60) % 60),
        reset_name(esp_reset_reason()), app->version, (unsigned)(esp_get_free_heap_size() / 1024),
        ota_health_slot(), ota_health_on_trial() ? "true" : "false", ota_health_rolled_back(),
        ota_health_known_good(), ota_health_known_good_version(), model,
        pic_prog_available() ? "true" : "false",
        json_esc(e_model, sizeof e_model, s_saved_model), json_esc(e_name, sizeof e_name, s_saved_name),
        json_esc(e_phost, sizeof e_phost, s_saved_host), s_set.airprint ? "true" : "false", s_set.lt_port,
        localtalk_serving() ? "true" : "false", lt_chip_name(s_lt_chip),
        dip, s_suffix, laser, printers, settings_fresh() ? "true" : "false",
        // Internal RAM is what Wi-Fi and lwIP live on: below ~20 KB, replies stall.
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)pst.doc_bytes, (unsigned)pst.printer_bytes, (unsigned)queued,
        (unsigned)pst.upload_ms, (unsigned)pst.rip_ms, (unsigned)pst.ms);
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
static void laser_web_json(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    laser_json(buf, cap);
}


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
    offers_apply();                 // W-1088: a way in on or off changes what Macs see
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

#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0 && CONFIG_AIRWRITER_PIC_DAT_PIN >= 0
// TashTalk took none of the sync, so the pins are free: U7's device ID
// tells an empty socket from a PIC without TashTalk, as the cable test does
// (W-1063). The read resets the PIC.
static lt_chip_t lt_chip_check(void)
{
    if (cable_test_lt_busy()) return LT_CHIP_SILENT;
    uint16_t id = 0;
    pic_err_t e = pic_prog_read_id(&id);
    ESP_LOGW(TAG, "LocalTalk: U7 reads 0x%04X: %s", id, pic_err_str(e));
    switch (e) {
    case PIC_ERR_NO_DEVICE:    return LT_CHIP_NONE;
    case PIC_OK:
    case PIC_ERR_WRONG_DEVICE: return LT_CHIP_NO_TASHTALK;
    default:                   return LT_CHIP_SILENT;
    }
}

static SemaphoreHandle_t s_lt_mx;   // one start or probe at a time: DIP task, page, boot

static localtalk_cfg_t lt_cfg(void)
{
    return (localtalk_cfg_t){
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
}
#endif

// W-1051: the LocalTalk node, in DIP mode 2 or on a bench build. It owns its
// UART and the PIC's pins from here until reboot, unless TashTalk is not
// there to take them (W-1081): then it does not start, and says why.
// probe_only asks U7 the same way and leaves the node off.
static void lt_go(const char *why, int probe_only)
{
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0 && CONFIG_AIRWRITER_PIC_DAT_PIN >= 0
    xSemaphoreTake(s_lt_mx, portMAX_DELAY);
    if (localtalk_running()) {
        xSemaphoreGive(s_lt_mx);
        return;
    }
    // A DIP flip mid cable test: let the test give UART2 back first.
    for (int i = 0; i < 30 && cable_test_lt_busy(); i++) vTaskDelay(pdMS_TO_TICKS(10));
    localtalk_cfg_t c = lt_cfg();
    esp_err_t err = probe_only ? localtalk_probe(&c) : localtalk_start(&c);
    if (err == ESP_ERR_NOT_FOUND) {
        s_lt_chip = lt_chip_check();
        ESP_LOGW(TAG, "LocalTalk (%s): %s", why, lt_chip_name(s_lt_chip));
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "LocalTalk (%s) did not start: %s", why, esp_err_to_name(err));
    } else {
        s_lt_chip = LT_CHIP_OK;
        ESP_LOGI(TAG, "LocalTalk %s (%s)", probe_only ? "chip runs TashTalk" : "on", why);
    }
    xSemaphoreGive(s_lt_mx);
#else
    (void)why;
    (void)probe_only;
#endif
}

static void lt_start(const char *why) { lt_go(why, 0); }

// Whether the node should run: the page's switch once it has been used, else
// DIP mode 2. A bench build always runs it, quiet while switched off.
static int lt_wanted(void)
{
#if CONFIG_AIRWRITER_LT_BENCH
    return 1;
#else
    return s_set.lt_port >= 0 ? s_set.lt_port == 1 : s_dip_known && s_dip.mode == 2;
#endif
}

static SemaphoreHandle_t s_offers_mx;   // offers_apply, one caller at a time

// W-1088: what old Macs see on each AppleTalk way in: on one that is on, every
// printer AirWriter can carry there; on one that is off, only the AirWriter
// marker (a link may run for a printer on it). Again whenever the printer list
// or a way-in switch changes.
static void offers_apply(void)
{
    // Called from httpd, the laser, kick and rip tasks and app_main: one at a
    // time, so no link gets another's offers (review I4).
    static lt_offers_t *o;
    xSemaphoreTake(s_offers_mx, portMAX_DELAY);
    if (!o) o = heap_caps_calloc(1, sizeof *o, MALLOC_CAP_SPIRAM);
    if (!o) {
        xSemaphoreGive(s_offers_mx);
        return;
    }
    int tunnel = 0, radio = 0, infinite = 0;
    char zone[40];
    if (ethertalk_running()) ethertalk_settings(&tunnel, &radio, &infinite, zone, sizeof zone);
    printers_offers(AT_LINK_CABLE, lt_wanted(), s_set.hostname, o);
    localtalk_set_offers(o);
    if (ethertalk_running()) {
        printers_offers(AT_LINK_TUNNEL, tunnel, s_set.hostname, o);
        ethertalk_set_offers(AT_LINK_TUNNEL, o);
        printers_offers(AT_LINK_ZONE, infinite, s_set.hostname, o);
        ethertalk_set_offers(AT_LINK_ZONE, o);
        printers_offers(AT_LINK_RADIO, radio, s_set.hostname, o);
        ethertalk_set_offers(AT_LINK_RADIO, o);
    }
    xSemaphoreGive(s_offers_mx);
}

// W-1082: whether a LaserWriter needs the cable (laser.c's links callback).
static volatile int s_laser_cable;

// The cable's node runs while the ImageWriter is shared there (the AppleTalk
// Port card's switch, W-1079) or the printers need it (laser.c: always, for
// the lookups). It cannot stop once started; it answers as the ImageWriter
// only while the switch says so.
static void lt_roles_apply(void)
{
    int serve = lt_wanted();
    if (!serve && !s_laser_cable) {
        if (localtalk_running()) localtalk_set_roles(0, 0);
        return;
    }
    if (!localtalk_running()) lt_start(serve ? "the settings page" : "the printers");
    localtalk_set_roles(serve, s_laser_cable);
    offers_apply();
}

// W-1079: the AppleTalk port's switch. Off quiets a running node (it cannot
// stop) unless a LaserWriter needs it; on starts one, or wakes a quiet one.
static void lt_port_apply(int on)
{
    (void)on;
    lt_roles_apply();
}

// laser.c: which links a LaserWriter needs, on its own task.
static void laser_links(int cable, int tunnel, int radio)
{
    s_laser_cable = cable;
    lt_roles_apply();
    ethertalk_set_laser(AT_LINK_TUNNEL, tunnel);
    ethertalk_set_laser(AT_LINK_RADIO, radio);
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
        snprintf(buf, cap, "{\"ok\":false,\"err\":\"TashTalk is already installed\"}");
        return;
    }
    // A cable test borrows the same pins, under the job lock.
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        snprintf(buf, cap, "{\"ok\":false,\"err\":\"The printer is busy. Try again when "
                 "it’s done\"}");
        return;
    }
    pic_result_t r;
    on_activity();
    led_grid_action(LG_ACT_PIC_FLASHING, 0);
    pic_err_t err = pic_prog_flash_tashtalk(&r);
    xSemaphoreGive(s_job_lock);
    led_grid_action(err == PIC_OK ? LG_ACT_PIC_OK : LG_ACT_PIC_FAILED, 0);
    pic_result_json(&r, buf, cap);
    // W-1081: TashTalk is on the chip now. A port that was wanted (or the
    // printers' lookups, W-1088) starts with no restart; otherwise U7 is only
    // asked, for the card.
    if (err == PIC_OK) {
        if (!lt_wanted() && !s_laser_cable) lt_go("TashTalk flashed", 1);
        else {
            lt_start("TashTalk flashed");
            lt_roles_apply();
        }
    }
}

// POST /api/cable. Refuses rather than waits while a job holds the port.
static void cable_web(void *ctx, int both, char *buf, size_t cap)
{
    (void)ctx;
    (void)both;
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0
    if (xSemaphoreTake(s_job_lock, 0) != pdTRUE) {
        snprintf(buf, cap, "{\"ok\":false,\"error\":\"The printer is busy. Try again when "
                 "it’s done\"}");
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
// W-1088: the port printer's own state, for AirPrint and old Macs. The panel's
// machine follows the box while idle (ready while any shared printer is) and
// a LaserWriter during its job; the port's printer is offline when its own line
// is down, whatever else is ready.
static ps_state_t port_state(ps_state_t box)
{
    if (s_printing && !s_laser_owns) return box;      // the machine follows the port's line
    if (s_iw_ready == 0) return PS_OFFLINE;
    if (s_iw_ready < 0) return box;                    // not heard from yet
    // W-1116: an ImageWriter's line is up but ESC ? has not named its model yet: offline on the
    // phone until the answer is in (port_live reads the same), so Ready appears with the answer.
    if (!s_port_sw && !s_iw_named) return PS_OFFLINE;
    return box == PS_OFFLINE || box == PS_ATTENTION ? PS_READY : box;
}

// An ImageWriter whose line is down, idle: off, unplugged or deselected.
static const char IW_OFFLINE_WORDS[] = "Offline: check power, cable and SELECT";

// What the port's own state is, in the words of a port record's family, for its slot: an
// ImageWriter's line, or what a StyleWriter said itself (no paper, a jam, the cartridge, the
// cover..., sw_status.c's words; it has no SELECT; nothing before it has answered, so
// nothing is claimed). `st` is the port's state for the panel (port_state).
static void port_push_own(int slot, int kind, ps_state_t st)
{
    if (kind == PR_KIND_STYLEWRITER) {
        sw_ipp_t w = sw_state_ipp(s_sw_state);
        if (s_iw_ready < 0 || !w.reason) ipp_server_set_state_for(slot, NULL, NULL, 0, 0);
        else ipp_server_set_state_for(slot, w.reason, w.message, 1, 0);
        return;
    }
    // SPEC.md 3.9: a stall mid-job becomes a real alert on the phone.
    // Mid-job stall: media-empty, the real "out of paper" alert. The -error
    // suffix (RFC 8011 5.4.12) says it outright: a stopped printer MUST carry
    // an error reason, and a bare one is only "assumed" to be one.
    // Idle and not ready: printer off, unplugged or deselected: the registered
    // "other", which on a stopped printer reads as an error all the same.
    // (offline-report is CUPS's keyword, not the IANA registry's: it is kept
    // for a record that is offline because another printer is connected, or
    // the port is set to Something else, as the W-1116 design says, so the two
    // read apart on a phone.)
    if (st == PS_ATTENTION)
        ipp_server_set_stopped_for(slot, "media-empty-error", "Needs attention: check paper and SELECT");
    else if (st == PS_OFFLINE)
        ipp_server_set_stopped_for(slot, "other", IW_OFFLINE_WORDS);
    else
        ipp_server_set_stopped_for(slot, NULL, NULL);
}

// What a port record's printer says while it is not the one on the port (W-1116): offline,
// in its family's own words with nothing answering (as the printer on the port would), or
// because another printer is connected (`on`: its name) or the port is set to Something
// else, in printers_core's words (pr_port_say_words). It offers no colour unless its model
// could: the ribbon or cartridge read was another printer's.
static void port_push_offline(int slot, int kind, int model, int say, const char *on)
{
    if (!colour_offered(kind, model)) ipp_server_set_colour_for(slot, 0);
    if (say != PR_PORT_SAY_OFFLINE) {
        char msg[PR_SAY_MAX];
        pr_port_say_words((pr_port_say_t)say, on, msg, sizeof msg);
        ipp_server_set_stopped_for(slot, "offline-report", msg);
        return;
    }
    if (kind == PR_KIND_STYLEWRITER) {
        sw_ipp_t w = sw_state_ipp(SWS_OFFLINE);
        ipp_server_set_stopped_for(slot, w.reason, w.message);
    } else {
        ipp_server_set_stopped_for(slot, "other", IW_OFFLINE_WORDS);
    }
}

// What phones and Macs are told about the printer port: every port record on AirPrint, by
// its slot (W-1116): the printer on the port its own state and the colour it offers (the
// ribbon or cartridge last read), every other port record offline; and the status word the
// AppleTalk nodes answer. Small on the stack: the 4 KB tasks call it (the port task, a
// StyleWriter session's).
static void port_push(ps_state_t box)
{
    ps_state_t st = port_state(box);
    printers_port_slot_t ps[PR_MAX];
    char on[PR_NAME_MAX];
    int n = printers_port_slots(ps, PR_MAX, on);
    for (int i = 0; i < n; i++) {
        const printers_port_slot_t *s = &ps[i];
        if (s->say == PR_PORT_SAY_OWN) {
            port_push_own(s->slot, s->kind, st);
            ipp_server_set_colour_for(s->slot, s_ribbon == IW_RIBBON_COLOUR &&
                                               colour_offered(s->kind, s->model));
        } else {
            port_push_offline(s->slot, s->kind, s->model, s->say, on);
        }
    }
    localtalk_set_status(lt_status(st, AT_LINK_CABLE));
    ethertalk_set_status(lt_status(st, AT_LINK_TUNNEL), lt_status(st, AT_LINK_ZONE),
                         lt_status(st, AT_LINK_RADIO));
}

// printers_cfg_t.placed: a record was put on AirPrint, moved or taken off, perhaps after the
// job that held its slot: its slot starts with no state, so every port slot is told again.
static void port_placed(void)
{
    port_push(printer_state_view().state);
}

static void on_ps_change(const ps_view_t *v)
{
    ESP_LOGI(TAG, "state -> %s", ps_name(v->state));
    if (s_boot_reset && v->state != PS_BOOTING) {
        s_boot_reset = 0;
        led_grid_action(LG_ACT_RESET_CONFIRM, 0);
    }
    port_push(v->state);
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
    s_offers_mx = xSemaphoreCreateMutex();
#if CONFIG_AIRWRITER_PIC_CLK_PIN >= 0 && CONFIG_AIRWRITER_PIC_DAT_PIN >= 0
    s_lt_mx = xSemaphoreCreateMutex();
#endif

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
    memdiag_start();                        // W-1094: what holds internal RAM (/api/mem)
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
    // W-1088: a board's first boot on the printer list. One set up before
    // (Wi-Fi saved, no list) keeps its host name and DIP rule, and its records
    // are made from what it had; a new one is airwriter-xxxx, port on.
    int has_wifi = s_set.ssid[0] != 0;
    int updated = has_wifi && !printers_stored();
    settings_first_boot(pr_default_host_fmt(has_wifi), s_suffix, pr_default_lt_port(has_wifi));
    settings_load(&s_set, s_suffix);
    printers_cfg_t pc = {
        .epoch = CONFIG_AIRWRITER_LASER_UUID_EPOCH, .updated_board = updated,
        .legacy = { .model = s_set.model, .name = s_set.name, .airprint = s_set.airprint,
                    .dither = s_set.dither, .gain = s_set.gain, .top_margin = s_set.top_margin,
                    .ribbon = s_set.ribbon },
        .port_sink = { .claim = doc_claim, .begin = doc_begin, .data = doc_data, .end = doc_end,
                       .cancel = doc_cancel, .ctx = NULL },
        .at_sink   = { .claim = doc_claim, .begin = laser_doc_begin, .data = doc_data,
                       .end = laser_doc_end, .cancel = laser_doc_cancel },
        .port_live = port_live, .port_screen = port_screen, .look = printers_look,
        .changed = printers_changed, .placed = port_placed,          // W-1116: placed
        .port_model = port_model_changed,                            // W-1116: R10's re-ask
        .clean = sw_clean_web, .confirm_sw = sw_confirm_queue,      // W-1103
    };
    memcpy(pc.mac, mac, sizeof pc.mac);
    ESP_ERROR_CHECK(printers_start(&pc));
    s_port_sw = printers_port_is_stylewriter();         // W-1103: whose answers are the port's state
    if (updated && !s_set.airprint) {
        // W-1079's switch was the ImageWriter's alone: its record took it as
        // Share, and the switch is now the whole box's.
        settings_save_switches(1, -1);
        s_set.airprint = 1;
    }
    {
        char dither[sizeof s_set.dither];
        int gain;
        if (!printers_port_screen(dither, sizeof dither, &gain)) {
            snprintf(s_set.dither, sizeof s_set.dither, "%s", dither);
            s_set.gain = (uint8_t)gain;
        }
        // The Chooser's name and mDNS's default instance: the port's one ImageWriter for
        // vintage Macs (W-1116: the oldest ImageWriter record's, fixed across swaps), else
        // the oldest port record's, else AirWriter.
        printers_chooser_name(s_set.queue, sizeof s_set.queue);
    }
    offers_apply();                 // the cable's, kept for its node's start
    s_ribbon = s_set.ribbon;
    // Wi-Fi saved before this restart: green greets the join (W-1049).
    if (settings_take_wifi_fresh()) printer_state_post(PS_EV_WIFI_FRESH);
    ota_health_start(s_set.ssid[0] != 0);
    snprintf(s_saved_model, sizeof s_saved_model, "%s", s_set.model);
    snprintf(s_saved_name, sizeof s_saved_name, "%s", s_set.name);
    snprintf(s_saved_host, sizeof s_saved_host, "%s", s_set.hostname);
#if CONFIG_AIRWRITER_LT_BENCH
    lt_start("bench build");
    if (s_set.lt_port == 0) lt_roles_apply();
#endif
    if (s_set.lt_port == 1) lt_start("the settings page");
    // W-1081: off or not, the card says whether U7 can run the port. DIP
    // mode 2, read later, starts the node as before.
    if (!localtalk_running()) lt_go("boot", 1);

    raster_selftest();
    // W-1082: no ImageWriter and a LaserWriter shared: the panel follows the
    // LaserWriter from the first report, not J2's line.


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
    // W-1103: the StyleWriter's line, over the port serial_out just brought up.
    stylewriter_cfg_t swc = { .record_is_sw = printers_port_is_stylewriter, .state = sw_state_cb,
                              .cart = sw_cart_seen, .cancelled = sw_cancelled,
                              .swapped = sw_swapped };
    stylewriter_start(&swc);
    // 4 KB: a ribbon change saves to NVS and re-announces mDNS from here; the StyleWriter's
    // questions and its model and cartridge saves run here too.
    xTaskCreate(kick_task, "kick", 4096, NULL, 3, NULL);

    job_pipe_cfg_t pipe = { .out = printer_out, .dither = algo, .gain = s_set.gain,
                            .top_margin = s_set.top_margin,
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
                              .cable_test = cable_web,     // no EtherTalk: the zone stays off the open AP
                              .printers_json = printers_web_json, .printers_post = printers_web_post };
        ESP_ERROR_CHECK(webui_start(&wcfg));
        ota_health_set(OH_WEB_UP, 1);
        // W-1088: setup shows what is plugged in before there is Wi-Fi: the
        // port answers as its line comes up, and the cable is looked on at once.
        laser_cfg_t lc = { .version = esp_app_get_description()->version, .first_look_ms = 0,
                           .links = laser_links, .found = laser_found, .queried = laser_queried,
                           .changed = laser_changed, .relay = laser_relay };
        if (laser_start(&lc) == ESP_OK) printers_apply();

        for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
    }

    ipp_server_cfg_t cfg = {
        .queue_name   = s_set.queue,
        .hostname     = s_set.hostname,
        .port         = 631,
        .port_printer = printers_port_ipp(),        // W-1088: as its record says
        .on_job_op    = on_activity,
        .colour       = s_ribbon == IW_RIBBON_COLOUR && ribbon_offered(),
    };
    ipp_server_set_airprint(s_set.airprint);          // W-1079
    ESP_ERROR_CHECK(ipp_server_start(&cfg));
    // The ribbon or the line may have been read while the server started; push what they are.
    port_push(printer_state_view().state);
    ota_health_set(OH_IPP_UP, 1);
    // wifi_start returned joined, before this could hear the join.
    fwu_start(0);
    fw_update_net_up();

    webui_cfg_t wcfg = { .status_json = status_json, .general_saved = general_saved, .captive = 0,
                          .restart = web_restart, .erase = web_erase, .test_page = web_test_page,
                          .localtalk_flash = localtalk_flash,
                          .lt_json = lt_json, .lt_job = lt_job, .lt_echo = lt_echo,
                          .cable_test = cable_web, .et_json = et_json,
                          .et_settings_get = et_settings_get, .et_settings_set = et_settings_set,
                          .laser_json = laser_web_json,
                          .printers_json = printers_web_json, .printers_post = printers_web_post };
    ESP_ERROR_CHECK(webui_start(&wcfg));
    ota_health_set(OH_WEB_UP, 1);
    et_start();
    offers_apply();

    // W-1082, W-1088: the printers on AppleTalk, each shared on its own slot.
    laser_cfg_t lc = { .version = esp_app_get_description()->version, .first_look_ms = 10000,
                       .links = laser_links, .found = laser_found, .queried = laser_queried,
                       .changed = laser_changed, .relay = laser_relay };
    err = laser_start(&lc);
    if (err != ESP_OK) ESP_LOGE(TAG, "AppleTalk printers did not start: %s", esp_err_to_name(err));
    printers_apply();                           // W-1116: the other slots are placed, and pushed (placed)

    if (s_test_on_boot) {
        s_test_on_boot = 0;
        ESP_LOGI(TAG, "DIP4 was on at boot; printing a test page");
        test_page();
    }

    ESP_LOGI(TAG, "ready: %s", ipp_server_uri());
    ESP_LOGI(TAG, "settings at http://%s.local/", s_set.hostname);
    ESP_LOGI(TAG, "%u bytes free after startup", (unsigned)esp_get_free_heap_size());
    memdiag_mark("ready");

    for (;;) vTaskDelay(pdMS_TO_TICKS(30000));
}
