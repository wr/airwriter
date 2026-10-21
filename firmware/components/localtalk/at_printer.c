// SPDX-License-Identifier: GPL-3.0-or-later
// The printer every AppleTalk link shares; see at_printer.h.
#include "at_printer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

static const char *TAG = "at";

#define JOB_CAP   (256 * 1024)      // PSRAM capture of the last PAP job
#define SPOOL     (16 * 1024)       // between the Mac and the printer; > LT_READ_MAX

// s_spool is single-producer (the holder's task, under its lock) and
// single-consumer (the print task), as a stream buffer must be: one job at a
// time means one producer at a time. s_job_active runs from begin until the
// print task has handed the last byte to the printer and called end; the rest
// are set by the holder's task.
static localtalk_printer_t s_printer;
static int          s_inited;
static StreamBufferHandle_t s_spool;
static TaskHandle_t s_print_task;
static const at_owner_t *volatile s_owner;
static volatile int s_job_active, s_job_ended, s_job_normal, s_job_failed;
static volatile int s_job_pending;           // begin is running for s_owner
static SemaphoreHandle_t s_begin_lock;       // links begin from their own tasks
static uint32_t     s_jobs_printed, s_jobs_failed;
static uint8_t     *s_job;
static size_t       s_job_len;
static uint32_t     s_job_total;
static int          s_job_done;

// Drains the spool into the printer. A failed write aborts the job (the Mac
// is told with CloseConn); an abandoned job's rest is dropped, not printed.
static void print_task(void *arg)
{
    (void)arg;
    uint8_t buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_spool, buf, sizeof buf, pdMS_TO_TICKS(100));
        if (n) {
            int drop = s_job_failed || (s_job_ended && !s_job_normal);
            if (!drop && s_printer.write && s_printer.write(s_printer.ctx, buf, n)) {
                s_job_failed = 1;
                const at_owner_t *o = s_owner;
                if (o && o->abort) o->abort(o->ctx);
            }
            continue;
        }
        if (s_job_active && s_job_ended && xStreamBufferIsEmpty(s_spool)) {
            int ok = s_job_normal && !s_job_failed;
            if (s_printer.end) ok = s_printer.end(s_printer.ctx, ok);
            if (ok) s_jobs_printed++; else s_jobs_failed++;
            s_job_active = 0;
            continue;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

esp_err_t at_printer_init(const localtalk_printer_t *p)
{
    if (s_inited) return ESP_OK;
    s_printer = *p;
    uint8_t *spool = heap_caps_malloc(SPOOL + 1, MALLOC_CAP_SPIRAM);
    if (!spool) spool = malloc(SPOOL + 1);              // a chip with no PSRAM
    static StaticStreamBuffer_t spool_cb;
    if (!spool) return ESP_ERR_NO_MEM;
    s_spool = xStreamBufferCreateStatic(SPOOL, 1, spool, &spool_cb);
    s_begin_lock = xSemaphoreCreateMutex();
    if (!s_begin_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreate(print_task, "lt_print", 4096, NULL, 3, &s_print_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_inited = 1;
    return ESP_OK;
}

int at_job_begin(const at_owner_t *o)
{
    // The last job's tail still draining counts as busy: one job at a time.
    if (!s_inited || s_job_active) return 1;
    xSemaphoreTake(s_begin_lock, portMAX_DELAY);   // begin never waits: this is short
    if (s_job_active) {
        xSemaphoreGive(s_begin_lock);
        return 1;
    }
    // The holder is named before main.c's begin runs: begin posts the job's
    // start, and each node's status word asks at_job_link who holds the
    // printer; a Mac told "busy" about its own job never sends (W-1051).
    s_owner = o;
    s_job_pending = 1;
    if (s_printer.begin && s_printer.begin(s_printer.ctx, o->link)) {
        s_job_pending = 0;
        s_owner = NULL;
        xSemaphoreGive(s_begin_lock);
        return 1;
    }
    s_job_ended = s_job_normal = s_job_failed = 0;
    s_job_active = 1;
    s_job_pending = 0;
    xSemaphoreGive(s_begin_lock);
    s_job_len = 0;
    s_job_total = 0;
    s_job_done = 0;
    xTaskNotifyGive(s_print_task);
    return 0;
}

void at_job_data(const at_owner_t *o, const uint8_t *d, size_t n)
{
    (void)o;
    if (!s_job) s_job = heap_caps_malloc(JOB_CAP, MALLOC_CAP_SPIRAM);
    if (s_job) {
        size_t take = s_job_len + n <= JOB_CAP ? n : JOB_CAP - s_job_len;
        memcpy(s_job + s_job_len, d, take);
        s_job_len += take;
    }
    s_job_total += (uint32_t)n;
    // The node asked for no more than at_job_room had, so this never blocks.
    size_t put = xStreamBufferSend(s_spool, d, n, 0);
    if (put != n) ESP_LOGE(TAG, "spool overrun: %u of %u bytes", (unsigned)put, (unsigned)n);
}

size_t at_job_room(const at_owner_t *o)
{
    (void)o;
    return s_inited ? xStreamBufferSpacesAvailable(s_spool) : 0;
}

void at_job_end(const at_owner_t *o, int normal)
{
    (void)o;
    s_job_normal = normal;
    s_job_ended = 1;
    s_job_done = 1;
    xTaskNotifyGive(s_print_task);
}

int at_job_link(void)
{
    const at_owner_t *o = s_owner;
    return (s_job_active || s_job_pending) && o ? (int)o->link : -1;
}

void at_printer_note(const char *line)
{
    const at_owner_t *o = s_owner;
    if (o && o->note) o->note(line);
}

const uint8_t *at_job_bytes(size_t *len)
{
    *len = s_job_len;
    return s_job_len ? s_job : NULL;
}

void at_printer_json(char *buf, size_t cap)
{
    snprintf(buf, cap,
        "\"job\":{\"captured\":%u,\"total\":%lu,\"done\":%s},"
        "\"print\":{\"active\":%s,\"spooled\":%u,\"failed\":%s,\"printed\":%lu,\"aborted\":%lu}",
        (unsigned)s_job_len, (unsigned long)s_job_total, s_job_done ? "true" : "false",
        s_job_active ? "true" : "false",
        s_inited ? (unsigned)xStreamBufferBytesAvailable(s_spool) : 0u,
        s_job_failed ? "true" : "false", (unsigned long)s_jobs_printed,
        (unsigned long)s_jobs_failed);
}
