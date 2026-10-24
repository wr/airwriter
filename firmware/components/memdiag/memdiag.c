// SPDX-License-Identifier: GPL-3.0-or-later
// W-1094: what holds internal RAM; see memdiag.h.
#include "memdiag.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "mem";

#define SAMPLE_MS   20          // two ticks at 100 Hz
#define SNAP_GAP_US 250000      // at most four low snapshots a second
#define MAX_TASKS   36
#define N_BUCKETS   11
#define N_BIG       12
#define N_FAIL      8

// Block sizes, upper bounds: bucket i holds [k_edge[i-1], k_edge[i]). Wi-Fi's
// and lwIP's frames are the 1.0-1.7 KB ones.
static const uint32_t k_edge[N_BUCKETS] = {
    64, 256, 512, 1024, 1536, 1700, 2048, 4096, 8192, 16384, UINT32_MAX,
};

// Tasks that live as long as the board does. A short-lived one (the rip, the
// test page, a reboot) is left out: its handle could be freed under us. Its
// stack still shows, as one of the biggest unattributed blocks.
static const char *const k_tasks[] = {
    "main", "ipp0", "ipp1", "ipp2", "ipp3", "httpd", "ethertalk", "websocket_task",
    "localtalk", "lt_print", "laser", "fw_update", "kick", "ps_poll", "ser_line", "led_grid",
    "dip_config", "prn", "memdiag", "captivedns",
    "tiT", "wifi", "sys_evt", "esp_timer", "Tmr Svc", "mdns", "ipc0", "ipc1",
    "IDLE0", "IDLE1",
};
#define N_NAMES (sizeof k_tasks / sizeof k_tasks[0])

typedef struct {
    const char *name;           // k_tasks'
    uint32_t    size;           // its stack's heap block; 0 if not in an internal heap
    uint32_t    spare;          // never-used stack, bytes
    uint8_t     psram;
} task_t;

typedef struct {
    int64_t  at_us;
    uint32_t free, largest, lowest, total;
    uint32_t used_n[N_BUCKETS], used_b[N_BUCKETS];   // used blocks, task stacks apart
    uint32_t free_n[N_BUCKETS], free_b[N_BUCKETS];
    uint32_t stacks_n, stacks_b;                     // internal stacks of the tasks below
    uint32_t big[N_BIG];                             // biggest used blocks, no known stack
    uint32_t fails;
    uint16_t ntasks;
    task_t   task[MAX_TASKS];
} snap_t;

typedef struct {
    snap_t     *s;
    const void *stk[MAX_TASKS];  // the internal stacks to recognise, and whose they are
    uint8_t     who[MAX_TASKS];
    int         n;
} walk_t;

// The failure ring: written by the allocator's own IRAM path, so it stays in
// internal RAM, and the hook touches nothing in flash.
typedef struct {
    uint32_t size, caps, at_ms;
    char     task[12];
} fail_t;
static DRAM_ATTR fail_t s_fail[N_FAIL];
static DRAM_ATTR volatile uint32_t s_fails;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_snap_mx;       // s_low and s_now
static snap_t *s_low, *s_now, *s_work;    // PSRAM
static int     s_low_valid;
static uint32_t s_low_free = UINT32_MAX;  // free RAM when s_low was taken
static int64_t  s_low_at;

// The window since the last mark, kept by the sampler.
static uint32_t    s_win_low = UINT32_MAX;
static const char *s_win_why = "boot";
static int64_t     s_win_at;

static IRAM_ATTR void on_fail(size_t size, uint32_t caps, const char *function_name)
{
    (void)function_name;
    uint32_t i = s_fails++ % N_FAIL;       // racy across cores; diagnostics
    s_fail[i].size = (uint32_t)size;
    s_fail[i].caps = caps;
    s_fail[i].at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    // FreeRTOS is in IRAM here (CONFIG_FREERTOS_IN_IRAM); no string calls.
    const char *n = xPortInIsrContext() ? "isr" : pcTaskGetName(NULL);
    int k = 0;
    for (; n && n[k] && k < (int)sizeof s_fail[i].task - 1; k++) s_fail[i].task[k] = n[k];
    s_fail[i].task[k] = '\0';
}

static int bucket(uint32_t sz)
{
    int k = 0;
    while (sz >= k_edge[k]) k++;
    return k;
}

// Inside the heap's critical section: counts only.
static bool walk_cb(walker_heap_into_t heap, walker_block_info_t b, void *arg)
{
    (void)heap;
    walk_t *w = arg;
    snap_t *s = w->s;
    uint32_t sz = (uint32_t)b.size;
    int k = bucket(sz);
    if (!b.used) {
        s->free_n[k]++;
        s->free_b[k] += sz;
        return true;
    }
    for (int i = 0; i < w->n; i++) {
        if (b.ptr == w->stk[i]) {
            s->task[w->who[i]].size = sz;
            s->stacks_n++;
            s->stacks_b += sz;
            return true;
        }
    }
    s->used_n[k]++;
    s->used_b[k] += sz;
    if (sz > s->big[N_BIG - 1]) {
        int j = N_BIG - 1;
        while (j > 0 && s->big[j - 1] < sz) {
            s->big[j] = s->big[j - 1];
            j--;
        }
        s->big[j] = sz;
    }
    return true;
}

static void take(snap_t *s)
{
    memset(s, 0, sizeof *s);
    walk_t w = { .s = s };
    for (size_t i = 0; i < N_NAMES && s->ntasks < MAX_TASKS; i++) {
        TaskHandle_t h = xTaskGetHandle(k_tasks[i]);
        if (!h) continue;
        task_t *t = &s->task[s->ntasks];
        t->name = k_tasks[i];
        t->spare = (uint32_t)uxTaskGetStackHighWaterMark(h);
        const uint8_t *stk = pxTaskGetStackStart(h);
        t->psram = esp_ptr_external_ram(stk);
        if (!t->psram && w.n < MAX_TASKS) {
            w.stk[w.n] = stk;
            w.who[w.n] = (uint8_t)s->ntasks;
            w.n++;
        }
        s->ntasks++;
    }
    heap_caps_walk(MALLOC_CAP_INTERNAL, walk_cb, &w);
    s->at_us   = esp_timer_get_time();
    s->free    = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s->largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s->lowest  = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s->total   = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    s->fails   = s_fails;
}

static void sampler(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        portENTER_CRITICAL(&s_mux);
        if (f < s_win_low) s_win_low = f;
        portEXIT_CRITICAL(&s_mux);
        // A new low by 512 bytes, or any new low under 8 KB.
        if (f >= s_low_free || (s_low_free - f < 512 && f >= 8192)) continue;
        int64_t now = esp_timer_get_time();
        if (s_low_valid && now - s_low_at < SNAP_GAP_US) continue;
        take(s_work);
        if (s_work->free > f + 2048) continue;       // the burst had passed by the walk
        xSemaphoreTake(s_snap_mx, portMAX_DELAY);
        snap_t *t = s_low;
        s_low = s_work;
        s_work = t;
        s_low_valid = 1;
        s_low_free = f;
        s_low_at = now;
        xSemaphoreGive(s_snap_mx);
        if (f < 8192)
            ESP_LOGW(TAG, "new low: %" PRIu32 " free, largest %" PRIu32 "; /api/mem has what held it",
                     s_low->free, s_low->largest);
    }
}

void memdiag_start(void)
{
    heap_caps_register_failed_alloc_callback(on_fail);
    s_snap_mx = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM);
    s_low  = heap_caps_calloc(1, sizeof *s_low, MALLOC_CAP_SPIRAM);
    s_now  = heap_caps_calloc(1, sizeof *s_now, MALLOC_CAP_SPIRAM);
    s_work = heap_caps_calloc(1, sizeof *s_work, MALLOC_CAP_SPIRAM);
    s_win_at = esp_timer_get_time();
    if (!s_snap_mx || !s_low || !s_now || !s_work) {
        ESP_LOGE(TAG, "no PSRAM for the memory diagnostics");
        return;
    }
    // Its stack in PSRAM: the sampler touches no flash. Priority 1, under
    // everything that does real work.
    if (xTaskCreateWithCaps(sampler, "memdiag", 3072, NULL, 1, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "could not start the sampler");
}

void memdiag_mark(const char *why)
{
    uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t big = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    uint32_t lo = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    uint32_t wl = s_win_low < f ? s_win_low : f;
    const char *prev = s_win_why;
    int64_t at = s_win_at;
    s_win_low = f;
    s_win_why = why;
    s_win_at = now;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "%s: free %" PRIu32 ", largest %" PRIu32 "; low %" PRIu32 " since %s %" PRIu32
             " ms before; lowest %" PRIu32 ", %" PRIu32 " failed allocations",
             why, f, big, wl, prev, (uint32_t)((now - at) / 1000), lo, (uint32_t)s_fails);
}

void memdiag_reset_low(void)
{
    if (!s_snap_mx) return;
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    s_low_valid = 0;
    s_low_free = UINT32_MAX;
    xSemaphoreGive(s_snap_mx);
}

static int put(char *buf, size_t cap, size_t *k, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
static int put(char *buf, size_t cap, size_t *k, const char *fmt, ...)
{
    if (*k >= cap) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *k, cap - *k, fmt, ap);
    va_end(ap);
    if (n < 0) return 0;
    *k += (size_t)n < cap - *k ? (size_t)n : cap - *k - 1;
    return 1;
}

static void arr(char *buf, size_t cap, size_t *k, const char *key, const uint32_t *v, int n)
{
    put(buf, cap, k, "\"%s\":[", key);
    for (int i = 0; i < n; i++) put(buf, cap, k, "%s%" PRIu32, i ? "," : "", v[i]);
    put(buf, cap, k, "]");
}

static void snap_json(char *buf, size_t cap, size_t *k, const snap_t *s, int64_t now)
{
    put(buf, cap, k,
        "{\"ago_ms\":%" PRIu32 ",\"free\":%" PRIu32 ",\"largest\":%" PRIu32 ",\"lowest\":%" PRIu32
        ",\"total\":%" PRIu32 ",\"fails\":%" PRIu32 ",\"stacks\":{\"n\":%" PRIu32 ",\"bytes\":%" PRIu32 "},",
        (uint32_t)((now - s->at_us) / 1000), s->free, s->largest, s->lowest, s->total, s->fails,
        s->stacks_n, s->stacks_b);
    arr(buf, cap, k, "edges", k_edge, N_BUCKETS - 1);
    put(buf, cap, k, ",");
    arr(buf, cap, k, "used_n", s->used_n, N_BUCKETS);
    put(buf, cap, k, ",");
    arr(buf, cap, k, "used_b", s->used_b, N_BUCKETS);
    put(buf, cap, k, ",");
    arr(buf, cap, k, "free_n", s->free_n, N_BUCKETS);
    put(buf, cap, k, ",");
    arr(buf, cap, k, "free_b", s->free_b, N_BUCKETS);
    put(buf, cap, k, ",");
    arr(buf, cap, k, "big", s->big, N_BIG);
    put(buf, cap, k, ",\"tasks\":[");
    for (int i = 0; i < s->ntasks; i++) {
        const task_t *t = &s->task[i];
        put(buf, cap, k, "%s{\"n\":\"%s\",\"stack\":%" PRIu32 ",\"spare\":%" PRIu32 ",\"in\":\"%s\"}",
            i ? "," : "", t->name, t->size, t->spare, t->psram ? "psram" : "internal");
    }
    put(buf, cap, k, "]}");
}

size_t memdiag_json(char *buf, size_t cap)
{
    size_t k = 0;
    if (!cap) return 0;
    buf[0] = '\0';
    if (!s_snap_mx) {
        put(buf, cap, &k, "{\"on\":false}");
        return k;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_snap_mx, portMAX_DELAY);
    take(s_now);
    put(buf, cap, &k, "{\"now\":");
    snap_json(buf, cap, &k, s_now, now);
    put(buf, cap, &k, ",\"low\":");
    if (s_low_valid) snap_json(buf, cap, &k, s_low, now);
    else put(buf, cap, &k, "null");
    xSemaphoreGive(s_snap_mx);

    portENTER_CRITICAL(&s_mux);
    uint32_t wl = s_win_low;
    const char *why = s_win_why;
    int64_t at = s_win_at;
    portEXIT_CRITICAL(&s_mux);
    put(buf, cap, &k, ",\"window\":{\"since\":\"%s\",\"ago_ms\":%" PRIu32 ",\"low\":%" PRIu32 "}",
        why, (uint32_t)((now - at) / 1000), wl);

    uint32_t nf = s_fails;
    put(buf, cap, &k, ",\"failed\":{\"count\":%" PRIu32 ",\"last\":[", nf);
    uint32_t now_ms = (uint32_t)(now / 1000);
    for (uint32_t j = 0, n = nf < N_FAIL ? nf : N_FAIL; j < n; j++) {
        fail_t f = s_fail[(nf - 1 - j) % N_FAIL];          // newest first
        f.task[sizeof f.task - 1] = '\0';
        put(buf, cap, &k, "%s{\"size\":%" PRIu32 ",\"caps\":\"0x%" PRIx32 "\",\"task\":\"%s\",\"ago_ms\":%" PRIu32 "}",
            j ? "," : "", f.size, f.caps, f.task, now_ms - f.at_ms);
    }
    put(buf, cap, &k, "]}}");
    return k;
}
