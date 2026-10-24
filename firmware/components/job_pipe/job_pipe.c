// SPDX-License-Identifier: GPL-3.0-or-later
#include "job_pipe.h"
#include "sdkconfig.h"
#include "ripjob.h"
#include "urf_decode.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "job";

// The job buffer, in PSRAM. job_pipe.h has why the upload should not wait on
// the paper, and how an ImageWriter job splits it.
#define POOL_BYTES   (1024 * 1024)
// An ImageWriter job's printer bytes, the pool's second half: a page of
// colour noise, the worst there is, rips to 460 KB of them, a full-page photo
// to 250-350 KB, a gray page to at most 115 KB (W-1066).
#define OUT_BYTES    (512 * 1024)
// The fallback with no PSRAM: big enough that a normal WiFi burst does not
// stall on every packet, small enough not to matter in internal RAM.
#define RING_BYTES   8192
// The rip, then job_finished's flush and logging on top of it since W-1042:
// a canceled iOS page left 944 bytes spare at 6144 on the bench.
#define PIPE_STACK   7168
// The printer task's: printer_out's resync and logging over one chunk. In
// PSRAM, as the task writes no flash, so it costs internal RAM nothing.
#define PRN_STACK    4096
#define PRN_CHUNK    512
// A paused job (paper out, SELECT) bigger than the ring backs the upload up
// behind the printer, so the push waits as long as the serial writer will,
// plus a margin.
#define PUSH_SECONDS (CONFIG_AIRWRITER_STALL_ABORT + 30)
#define PUSH_TIMEOUT pdMS_TO_TICKS(PUSH_SECONDS * 1000)

static job_pipe_cfg_t   s_cfg;
// Three rings over the one pool, in two layouts: a LaserWriter job's URF in
// all of it (s_whole), or an ImageWriter job's URF in the first half (s_urf)
// and its printer bytes in the second (s_out). Only one layout is in use at a
// time and every ring is empty between jobs, so what one wrote in another's
// storage is never read. Made once: making a ring fills its storage.
static StreamBufferHandle_t s_whole, s_urf, s_out;
static StaticStreamBuffer_t s_whole_ctl, s_urf_ctl, s_out_ctl;
static StreamBufferHandle_t s_ring;   // this job's URF: s_whole or s_urf
static int               s_buffered;  // this job's printer bytes go through s_out
static SemaphoreHandle_t s_printed;   // the printer task sent the job's last byte
static volatile int      s_out_eof;   // the rip is over: s_out gets no more
static TaskHandle_t      s_task;

static volatile int s_eof;        // producer has no more bytes
static volatile int s_aborted;    // transfer was truncated; abandon the page
static job_pipe_stats_t s_st;
static char s_name[64];
static job_pipe_target_t s_target;    // this job's printer, when not the ImageWriter
static int s_has_target;

// urf_decode's source. Returns 1 only when all n bytes were delivered, which
// is what makes a truncated document a decode error rather than a short page.
static int ring_read(void *ctx, uint8_t *buf, size_t n)
{
    (void)ctx;
    size_t got = 0;
    while (got < n) {
        if (s_aborted) return 0;
        size_t r = xStreamBufferReceive(s_ring, buf + got, n - got, pdMS_TO_TICKS(250));
        got += r;
        if (r == 0 && s_eof && xStreamBufferIsEmpty(s_ring)) return 0;
    }
    return 1;
}

static void pipe_log(void *ctx, const char *msg)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", msg);
}

// The rip's printer stream: into s_out for the printer task, or straight to
// cfg.out with no PSRAM. A full s_out is a printer 512 KB behind, which the
// rip waits out as it once waited on the serial port; the printer task gives
// up on a stalled printer and aborts the job, which ends the wait.
static void count_out(void *ctx, const uint8_t *buf, size_t n)
{
    (void)ctx;
    s_st.printer_bytes += n;
    if (!s_buffered) {
        if (s_cfg.out) s_cfg.out(s_cfg.out_ctx, buf, n);
        return;
    }
    size_t sent = 0;
    while (sent < n && !s_aborted)
        sent += xStreamBufferSend(s_out, buf + sent, n - sent, pdMS_TO_TICKS(250));
}

// The printer's side of an ImageWriter job: s_out to cfg.out at the printer's
// pace, while the rip runs ahead at the upload's (W-1066). One task for the
// board's life, idle between jobs.
static void prn_task(void *arg)
{
    (void)arg;
    uint8_t buf[PRN_CHUNK];
    for (;;) {
        size_t r = xStreamBufferReceive(s_out, buf, sizeof buf, pdMS_TO_TICKS(100));
        // An abandoned job's bytes go nowhere: its tail has been purged, and
        // the rip unwinds sooner with room to write.
        if (r && !s_aborted && s_cfg.out) s_cfg.out(s_cfg.out_ctx, buf, r);
        // Only the rip fills s_out, and it is done: empty now is empty for good.
        if (s_out_eof && xStreamBufferIsEmpty(s_out)) {
            s_out_eof = 0;
            xSemaphoreGive(s_printed);
        }
    }
}

static void pipe_task(void *arg)
{
    (void)arg;
    int64_t t0 = esp_timer_get_time();

    urf_t u;
    int rc = urf_open(&u, ring_read, NULL);
    if (rc != URF_OK) {
        ESP_LOGE(TAG, "\"%s\": not a URF stream (%d)%s", s_name, rc,
                 u.is_pwg ? " -- looks like PWG raster, which is not supported" : "");
        s_st.rc = rc;
    } else {
        ripjob_cfg_t base = {
            .out     = count_out,
            .dither  = s_cfg.dither,
            .gain    = s_cfg.gain,
            .top_margin = s_cfg.top_margin,
            .log     = pipe_log,
        };
        ripjob_cfg_t cfg = job_pipe_rip_cfg(base, s_cfg.colour_ok, s_cfg.colour_ctx,
                                            s_has_target ? &s_target : NULL);
        ripjob_stats_t rst;
        memset(&rst, 0, sizeof rst);
        s_st.rc     = ripjob_run(&u, &cfg, &rst);
        s_st.pages  = rst.pages;
        s_st.dst_w  = rst.dst_w;
        s_st.dst_h  = rst.dst_h;
    }
    urf_free(&u);
    s_st.rip_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (s_buffered)
        ESP_LOGI(TAG, "rip done in %u ms; %u KB of %u still to print",
                 (unsigned)s_st.rip_ms, (unsigned)(xStreamBufferBytesAvailable(s_out) / 1024),
                 (unsigned)(s_st.printer_bytes / 1024));

    // Whatever happened, drain the rest so the producer is never left blocked
    // on a full ring with nobody reading.
    uint8_t junk[256];
    while (!s_eof || !xStreamBufferIsEmpty(s_ring)) {
        if (xStreamBufferReceive(s_ring, junk, sizeof junk, pdMS_TO_TICKS(250)) == 0
            && s_eof) break;
    }

    // Then the printer: the job is over when its last byte has gone to cfg.out.
    if (s_buffered) {
        s_out_eof = 1;
        xSemaphoreTake(s_printed, portMAX_DELAY);
        // Abandoned after the rip, mid-print: the rest went nowhere.
        if (s_aborted && s_st.rc == 0) s_st.rc = -1;
    }

    s_st.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    job_pipe_stats_t st = s_st;

    // Free for the next job before reporting: `done` is where the caller gives
    // the printer back, and the next job_pipe_begin may come the moment it
    // does. Nothing below touches the shared state.
    s_task = NULL;
    if (s_cfg.done) s_cfg.done(s_cfg.done_ctx, &st);
    // `done` runs here too now, so watch the margin.
    ESP_LOGI(TAG, "rip task: %u bytes of stack to spare",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

esp_err_t job_pipe_init(const job_pipe_cfg_t *cfg)
{
    s_cfg = *cfg;

    // Static, because the storage has to come from PSRAM explicitly: a plain
    // xStreamBufferCreate would put a 1MB ring in internal RAM, or fail.
    uint8_t *pool = heap_caps_malloc(POOL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pool) s_printed = xSemaphoreCreateBinaryWithCaps(MALLOC_CAP_SPIRAM);
    if (s_printed) {
        s_whole = xStreamBufferCreateStatic(POOL_BYTES, 1, pool, &s_whole_ctl);
        s_urf   = xStreamBufferCreateStatic(POOL_BYTES - OUT_BYTES, 1, pool, &s_urf_ctl);
        s_out   = xStreamBufferCreateStatic(OUT_BYTES, 1, pool + POOL_BYTES - OUT_BYTES,
                                            &s_out_ctl);
        // Above the rip's 4, so a busy rip never keeps the printer waiting.
        if (xTaskCreateWithCaps(prn_task, "prn", PRN_STACK, NULL, 5, NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            ESP_LOGI(TAG, "job buffer %u KB in PSRAM: %u KB of URF and %u KB to print "
                     "for the ImageWriter", POOL_BYTES / 1024,
                     (POOL_BYTES - OUT_BYTES) / 1024, OUT_BYTES / 1024);
            return ESP_OK;
        }
        vSemaphoreDeleteWithCaps(s_printed);
        s_printed = NULL;
        s_out = NULL;
    }

    heap_caps_free(pool);
    s_whole = s_urf = xStreamBufferCreate(RING_BYTES, 1);
    ESP_LOGW(TAG, "no PSRAM for the job buffer; streaming through %u bytes", RING_BYTES);
    return s_whole ? ESP_OK : ESP_ERR_NO_MEM;
}

void job_pipe_set_screen(ht_algo_t dither, int gain)
{
    s_cfg.dither = dither;
    s_cfg.gain   = gain;
}

void job_pipe_set_top_margin(int top)
{
    s_cfg.top_margin = top;
}

esp_err_t job_pipe_begin(const char *job_name)
{
    return job_pipe_begin_to(job_name, NULL);
}

esp_err_t job_pipe_begin_to(const char *job_name, const job_pipe_target_t *t)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    s_has_target = t != NULL;
    if (t) s_target = *t;
    // Another printer's rows go to its own stage, never to cfg.out, so its
    // job keeps the whole pool for URF, as every job did before W-1066.
    s_buffered = !t && s_out;
    s_ring = s_buffered ? s_urf : s_whole;

    // The last job's task drained the ring before it finished, so this is
    // belt and braces. Not xStreamBufferReset: with assertions on it memsets
    // the whole 1MB inside a critical section, interrupts off.
    uint8_t junk[64];
    while (xStreamBufferReceive(s_ring, junk, sizeof junk, 0) > 0) {}
    memset(&s_st, 0, sizeof s_st);
    s_eof = s_aborted = 0;
    strncpy(s_name, job_name && job_name[0] ? job_name : "Untitled", sizeof s_name - 1);
    s_name[sizeof s_name - 1] = '\0';

    if (xTaskCreate(pipe_task, "rip", PIPE_STACK, NULL, 4, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t job_pipe_data(const uint8_t *buf, size_t n)
{
    s_st.doc_bytes += n;

    size_t sent = 0;
    while (sent < n) {
        if (s_aborted) return ESP_FAIL;
        size_t w = xStreamBufferSend(s_ring, buf + sent, n - sent, PUSH_TIMEOUT);
        if (w == 0) {
            ESP_LOGE(TAG, "pipeline stalled for %ds; abandoning the job", PUSH_SECONDS);
            s_aborted = 1;
            return ESP_FAIL;
        }
        sent += w;
    }
    return ESP_OK;
}

void job_pipe_abort(void)
{
    if (!s_task) return;
    ESP_LOGW(TAG, "job canceled; abandoning the page");
    s_aborted = 1;
}

void job_pipe_end(int ok)
{
    if (!ok) s_aborted = 1;
    s_eof = 1;
}
