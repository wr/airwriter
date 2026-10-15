// SPDX-License-Identifier: GPL-3.0-or-later
#include "job_pipe.h"
#include "sdkconfig.h"
#include "ripjob.h"
#include "urf_decode.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

static const char *TAG = "job";

// Big enough that a normal WiFi burst does not stall on every packet, small
// enough that it is not a page buffer in disguise. A Letter page decodes to
// 1.9MB; nothing here ever holds more than this.
#define RING_BYTES   8192
#define PIPE_STACK   6144
// A paused job (paper out, SELECT) backs the upload up behind the printer, so
// the push waits as long as the serial writer will, plus a margin.
#define PUSH_SECONDS (CONFIG_AIRWRITER_STALL_ABORT + 30)
#define PUSH_TIMEOUT pdMS_TO_TICKS(PUSH_SECONDS * 1000)

static job_pipe_cfg_t   s_cfg;
static StreamBufferHandle_t s_ring;
static SemaphoreHandle_t s_done;
static TaskHandle_t      s_task;

static volatile int s_eof;        // producer has no more bytes
static volatile int s_aborted;    // transfer was truncated; abandon the page
static job_pipe_stats_t s_st;
static char s_name[64];

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

static void count_out(void *ctx, const uint8_t *buf, size_t n)
{
    s_st.printer_bytes += n;
    if (s_cfg.out) s_cfg.out(s_cfg.out_ctx, buf, n);
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
        ripjob_cfg_t cfg = {
            .out     = count_out,
            .dither  = s_cfg.dither,
            .log     = pipe_log,
        };
        ripjob_stats_t rst;
        memset(&rst, 0, sizeof rst);
        s_st.rc     = ripjob_run(&u, &cfg, &rst);
        s_st.pages  = rst.pages;
        s_st.dst_w  = rst.dst_w;
        s_st.dst_h  = rst.dst_h;
    }
    urf_free(&u);

    // Whatever happened, drain the rest so the producer is never left blocked
    // on a full ring with nobody reading.
    uint8_t junk[256];
    while (!s_eof || !xStreamBufferIsEmpty(s_ring)) {
        if (xStreamBufferReceive(s_ring, junk, sizeof junk, pdMS_TO_TICKS(250)) == 0
            && s_eof) break;
    }

    s_st.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    xSemaphoreGive(s_done);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t job_pipe_init(const job_pipe_cfg_t *cfg)
{
    s_cfg  = *cfg;
    s_ring = xStreamBufferCreate(RING_BYTES, 1);
    s_done = xSemaphoreCreateBinary();
    return (s_ring && s_done) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t job_pipe_begin(const char *job_name)
{
    if (s_task) return ESP_ERR_INVALID_STATE;

    xStreamBufferReset(s_ring);
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

void job_pipe_end(int ok, job_pipe_stats_t *st)
{
    if (!ok) s_aborted = 1;
    s_eof = 1;

    // A Letter page is 119 seconds of transmission at 9600 baud, and the
    // pipeline blocks on the UART the whole time. Waiting 30s here would
    // abandon every real job a third of the way through.
    for (int waited = 0; !xSemaphoreTake(s_done, pdMS_TO_TICKS(15000)); waited += 15) {
        if (waited >= CONFIG_AIRWRITER_STALL_ABORT + 600) { ESP_LOGE(TAG, "pipeline stuck past the stall limit"); break; }
        ESP_LOGI(TAG, "still printing: %us elapsed, %u bytes out",
                 (unsigned)waited + 15, (unsigned)s_st.printer_bytes);
    }

    if (st) *st = s_st;
}
