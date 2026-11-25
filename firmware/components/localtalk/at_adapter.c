// SPDX-License-Identifier: GPL-3.0-or-later
// A Mac's StyleWriter job, as every AppleTalk link shares it; see at_adapter.h.
#include "at_adapter.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sw_pipe.h"

static const char *TAG = "at";

#define PUMP_STACK  4096
#define PUMP_CHUNK  256             // toward the port a round: 44 ms at 57600

// The pipe and the pump's scratch, in PSRAM, made at the first job.
typedef struct {
    sw_pipe_t pipe;
    uint8_t   buf[PUMP_CHUNK];
} swa_mem_t;

static at_swa_port_t     s_port;
static int               s_inited;
static SemaphoreHandle_t s_mx;              // the pipe's lock: held for a copy, never a write
static swa_mem_t        *s_mem;
// One job on the board. s_active runs from a claim that held to the pump's
// last line; the job lock (main.c's claim) is what lets one link in at a time.
static const at_swa_owner_t *volatile s_owner;
static volatile int      s_active;
static volatile int      s_stop;            // the test button; the pump acts
static TaskHandle_t      s_pump;

static void pipe_lock(void *ctx)   { (void)ctx; xSemaphoreTake(s_mx, portMAX_DELAY); }
static void pipe_unlock(void *ctx) { (void)ctx; xSemaphoreGive(s_mx); }

void at_swa_init(const at_swa_port_t *port)
{
    if (s_inited) return;
    s_mx = xSemaphoreCreateMutex();
    if (!s_mx) return;
    s_port = *port;
    s_inited = 1;
}

// ------------------------------------------------------------- the pump

static int io_write(void *ctx, const uint8_t *d, size_t n) { (void)ctx; return s_port.write(s_port.ctx, d, n); }
static int io_flush(void *ctx) { (void)ctx; return s_port.flush(s_port.ctx); }
static int io_read(void *ctx, uint8_t *buf, size_t cap, int idle)
{
    (void)ctx;
    return s_port.read(s_port.ctx, buf, cap, idle);
}

// Owns the port for one job. It waits for begin's books, then moves bytes
// until the adapter's end, or until the board's stop or a dead port has had
// the adapter end it.
static void pump_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const at_swa_owner_t *o = s_owner;
    sw_pipe_t *p = &s_mem->pipe;
    const sw_pump_io_t io = { io_write, s_port.flush ? io_flush : NULL, io_read, NULL };
    if (s_port.open(s_port.ctx)) sw_pipe_fail(p);       // nothing is sent; the Mac is told
    sw_pump_t v;
    for (;;) {
        if (s_stop) {
            s_stop = 0;
            sw_pipe_stop(p);
        }
        v = sw_pipe_pump(p, &io, s_mem->buf, sizeof s_mem->buf);
        // Under its link's lock, from here and never from that link's own task.
        if (v == SW_PUMP_STOP) o->stop(o->ctx);
        else if (v != SW_PUMP_GO) break;
    }
    at_swa_end_t e = { .clean = v == SW_PUMP_CLEAN, .stopped = sw_pipe_stopped(p),
                       .failed = sw_pipe_failed(p), .down = sw_pipe_down_total(p),
                       .up = sw_pipe_up_total(p), .reached = sw_pipe_reached(p) };
    s_port.close(s_port.ctx, &e);                       // gives the job lock
    ESP_LOGI(TAG, "adapter pump: %u bytes of stack to spare",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    s_stop = 0;
    s_owner = NULL;
    s_active = 0;
    vTaskDelete(NULL);
}

// ------------------------------------------------------------- an adapter's ops
// On its link's task with that link's lock held (swa_stop and swa_offer from
// other tasks too, under the same lock); ctx is its owner.

static int mine(const void *ctx) { return s_active && s_owner == ctx; }

static void t_send(void *ctx, const lt_route_t *to, uint8_t src_sock, const uint8_t *p, size_t n)
{
    const at_swa_owner_t *o = ctx;
    o->send(o->ctx, to, src_sock, p, n);
}

static void t_log(void *ctx, const char *line)
{
    const at_swa_owner_t *o = ctx;
    if (o->log) o->log(o->ctx, line);
}

// A job whose pump is still putting the port back counts: the printer is not free yet.
static int t_busy(void *ctx)
{
    (void)ctx;
    return s_active || !s_inited || s_port.busy(s_port.ctx);
}

static int t_begin(void *ctx, uint8_t id)
{
    const at_swa_owner_t *o = ctx;
    if (!s_inited || s_active) return 1;
    if (s_port.claim(s_port.ctx, o->link, id)) return 1;
    // The job lock is ours from here, so no other link is past this line.
    if (!s_mem) {
        s_mem = heap_caps_calloc(1, sizeof *s_mem, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_mem) sw_pipe_init(&s_mem->pipe, pipe_lock, pipe_unlock, NULL);
    }
    if (!s_mem) {
        s_port.unclaim(s_port.ctx);
        return 1;
    }
    // The pipe before the 0: the adapter asks its room for the call-back at once.
    sw_pipe_begin(&s_mem->pipe);
    size_t cap = 0;
    uint8_t *keep = at_capture_lend(&cap);
    sw_pipe_capture(&s_mem->pipe, keep, cap);
    s_stop = 0;
    s_owner = o;
    s_active = 1;
    // With internal RAM short a task cannot be made (docs/traps.md): then
    // nothing was begun, and the Mac hears busy and asks again.
    if (xTaskCreate(pump_task, "sw_pipe", PUMP_STACK, NULL, 3, &s_pump) != pdPASS) {
        s_owner = NULL;
        s_active = 0;
        s_port.unclaim(s_port.ctx);
        ESP_LOGW(TAG, "adapter: no memory for the job's task; busy");
        return 1;
    }
    s_port.begin(s_port.ctx);
    xTaskNotifyGive(s_pump);
    return 0;
}

static void t_up(void *ctx) { (void)ctx; }

static size_t t_room(void *ctx) { return mine(ctx) ? sw_pipe_room(&s_mem->pipe) : 0; }

static void t_data(void *ctx, const uint8_t *d, size_t n)
{
    if (mine(ctx)) sw_pipe_put(&s_mem->pipe, d, n);
}

static size_t t_output(void *ctx, uint8_t *buf, size_t max)
{
    return mine(ctx) ? sw_pipe_output(&s_mem->pipe, buf, max) : 0;
}

static int t_drained(void *ctx) { return !mine(ctx) || sw_pipe_drained(&s_mem->pipe); }

static void t_end(void *ctx, int clean)
{
    if (mine(ctx)) sw_pipe_end(&s_mem->pipe, clean);
}

// The name is a person's: converted and handed on, never logged.
static void t_rename(void *ctx, uint8_t id, const uint8_t *pname)
{
    (void)ctx;
    char name[3 * NBP_NAME_MAX + 1];
    nbp_utf8(name, sizeof name, pname);
    if (s_inited && s_port.rename && name[0]) s_port.rename(s_port.ctx, id, name);
}

sw_adapter_t *at_swa_make(const at_swa_owner_t *o, uint32_t seed)
{
    sw_adapter_t *a = heap_caps_calloc(1, sizeof *a, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a) return NULL;
    const swa_ops_t ops = {
        .ctx = (void *)o, .send = t_send, .log = t_log, .busy = t_busy, .job_begin = t_begin,
        .job_up = t_up, .job_room = t_room, .job_data = t_data, .job_output = t_output,
        .job_drained = t_drained, .job_end = t_end, .rename = t_rename,
    };
    swa_init(a, &ops, seed);
    return a;
}

int at_swa_stop(void)
{
    if (!s_active) return 0;
    s_stop = 1;
    return 1;
}

const uint8_t *at_swa_bytes(size_t *len)
{
    *len = 0;
    if (!s_mem || !at_capture_lent()) return NULL;
    return sw_pipe_captured(&s_mem->pipe, len);
}
