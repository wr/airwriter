// SPDX-License-Identifier: GPL-3.0-or-later
#include "logbook.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp32s3/rom/cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "xtensa_context.h"

// Warnings and errors go to flash at once during a job (a page write, no erase), so a stuck
// job followed by someone unplugging the board still leaves its reason.
#ifndef LB_SAVE_DURING_JOB
#define LB_SAVE_DURING_JOB 1
#endif

#define CRASH_MAGIC 0x43524153u
#define NAME_CAP    72

typedef struct {
    char        fmt[LB_TEXT_MAX + LB_TAG_MAX + 64];
    lb_line_t   line, skip;
    lb_asm_t    pieces;
    lb_store_t  store;
    lb_reader_t reader;
    char        network[NAME_CAP], host[NAME_CAP];
    char        first[LB_TEXT_MAX + 1];     // this start's first line, to begin again after an erase
} logbook_ps_t;

// Survives a restart: PSRAM's contents outlast a panic or esp_restart, and .noinit keeps
// startup from clearing them. A power cut leaves noise, which the magic number turns away.
EXT_RAM_NOINIT_ATTR static uint8_t s_ring_mem[LB_RING_BYTES] __attribute__((aligned(4)));
RTC_NOINIT_ATTR static struct { uint32_t magic; lb_crash_t c; } s_crash;

static logbook_ps_t       *s_ps;
static lb_ring_t          *s_ring;
static SemaphoreHandle_t   s_ring_lock, s_store_lock;   // always store, then ring
static int               (*s_busy)(void);
static vprintf_like_t      s_prev;
static int                 s_store_ok;
static volatile unsigned   s_errors;
static volatile int        s_dropped;

void __real_esp_panic_handler(panic_info_t *info);

static void record_crash(const panic_info_t *info)
{
    lb_crash_t *c = &s_crash.c;
    memset(c, 0, sizeof *c);
    const char *why = g_panic_abort ? (g_panic_abort_details ? g_panic_abort_details : "abort")
                    : info->reason ? info->reason
                    : info->description ? info->description : "unknown";
    strlcpy(c->reason, why, sizeof c->reason);
    const char *task = pcTaskGetName(NULL);
    strlcpy(c->task, task ? task : "?", sizeof c->task);
    c->core = (uint8_t)info->core;
    c->ms = (uint32_t)(esp_timer_get_time() / 1000);

    const XtExcFrame *f = info->frame;
    esp_backtrace_frame_t bt = { .pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = (void *)f };
    c->pc[c->n++] = esp_cpu_process_stack_pc(bt.pc);
    while (c->n < 8 && bt.next_pc && esp_backtrace_get_next_frame(&bt))
        c->pc[c->n++] = esp_cpu_process_stack_pc(bt.pc);

    s_crash.magic = CRASH_MAGIC;
    Cache_WriteBack_All();                  // the ring's newest lines are still in the data cache
}

// Runs with the other core stalled. With the cache off, nothing outside IRAM is safe, so the
// crash goes unrecorded and the panic carries on as it always has.
IRAM_ATTR void __wrap_esp_panic_handler(panic_info_t *info)
{
    if (spi_flash_cache_enabled()) record_crash(info);
    __real_esp_panic_handler(info);
}

static void on_shutdown(void)
{
    Cache_WriteBack_All();                  // a soft restart keeps the newest lines too
}

// On whichever task logged, including Wi-Fi's: nothing large on the stack, and never a long
// wait. Formats, then forwards to the console, one after the other rather than nested.
static int log_hook(const char *fmt, va_list ap)
{
    if (xPortCanYield() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING
        && xSemaphoreTake(s_ring_lock, 1) == pdTRUE) {      // one tick at most
        va_list copy;
        va_copy(copy, ap);
        vsnprintf(s_ps->fmt, sizeof s_ps->fmt, fmt, copy);
        va_end(copy);
        lb_line_t *l = &s_ps->line;
        if (lb_feed(&s_ps->pieces, s_ps->fmt, xTaskGetCurrentTaskHandle(), l)) {
            lb_names_t names = { s_ps->network, s_ps->host };
            lb_scrub(l, &names);
            l->ram_only = (uint8_t)lb_is_chatter(l);
            if (l->level == 'E') s_errors++;
            lb_ring_push(s_ring, l);
        }
        xSemaphoreGive(s_ring_lock);
    } else {
        s_dropped++;
    }
    return s_prev ? s_prev(fmt, ap) : 0;
}

static int fl_read(void *c, uint32_t off, void *buf, uint32_t n)
{
    return esp_partition_read(c, off, buf, n) != ESP_OK;
}

static int fl_write(void *c, uint32_t off, const void *buf, uint32_t n)
{
    return esp_partition_write(c, off, buf, n) != ESP_OK;
}

static int fl_erase(void *c, uint32_t sector)
{
    return esp_partition_erase_range(c, sector * LB_SECTOR_BYTES, LB_SECTOR_BYTES) != ESP_OK;
}

static void push_sys(char level, uint32_t ms, const char *text)
{
    lb_line_t *l = &s_ps->line;
    memset(l, 0, sizeof *l);
    l->level = level;
    l->ms = ms;
    strlcpy(l->tag, "sys", sizeof l->tag);
    strlcpy(l->text, text, sizeof l->text);
    lb_ring_push(s_ring, l);
}

void logbook_start(int (*busy)(void), const char *reset)
{
    s_busy = busy;
    s_ps = heap_caps_calloc(1, sizeof *s_ps, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ring_lock = xSemaphoreCreateMutex();
    s_store_lock = xSemaphoreCreateMutex();
    if (!s_ps || !s_ring_lock || !s_store_lock) {
        s_ps = NULL;                        // no log, and the board carries on without one
        return;
    }

    int kept = 0;
    s_ring = lb_ring_attach(s_ring_mem, sizeof s_ring_mem, &kept);

    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                        ESP_PARTITION_SUBTYPE_ANY, "storage");
    uint32_t last_seq = 0;
    uint16_t last_run = 0;
    if (p && p->size >= LB_SECTORS * LB_SECTOR_BYTES) {
        lb_flash_t f = { (void *)p, fl_read, fl_write, fl_erase };
        lb_store_open(&s_ps->store, &f);
        last_seq = s_ps->store.last_seq;
        last_run = s_ps->store.last_run;
        s_store_ok = 1;
    }

    if (s_crash.magic == CRASH_MAGIC) {
        s_crash.magic = 0;
        if (!kept) lb_ring_begin_run(s_ring, last_seq + 1, last_run ? last_run - 1 : 0);
        char text[LB_TEXT_MAX + 1];
        lb_crash_text(&s_crash.c, text, sizeof text);
        push_sys('E', s_crash.c.ms, text);
    }

    lb_ring_begin_run(s_ring, last_seq + 1, last_run);
    snprintf(s_ps->first, sizeof s_ps->first, "reset=%s version=%s", reset,
             esp_app_get_description()->version);
    push_sys('S', 0, s_ps->first);

    esp_register_shutdown_handler(on_shutdown);
    s_prev = esp_log_set_vprintf(log_hook);
}

void logbook_set_names(const char *network, const char *host)
{
    if (!s_ps) return;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    strlcpy(s_ps->network, network ? network : "", sizeof s_ps->network);
    strlcpy(s_ps->host, host ? host : "", sizeof s_ps->host);
    xSemaphoreGive(s_ring_lock);
}

static void save_pending(void)
{
    lb_store_t *st = &s_ps->store;
    for (;;) {
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        int more = lb_save_next(st, s_ring);
        xSemaphoreGive(s_ring_lock);            // never held across a flash write
        if (!more || !lb_store_append(st, &st->line)) break;
    }
}

void logbook_tick(void)
{
    if (!s_ps || !s_store_ok || xSemaphoreTake(s_store_lock, 0) != pdTRUE) return;
    if (!s_busy || !s_busy()) {
        lb_store_prepare(&s_ps->store);
        save_pending();
    } else if (LB_SAVE_DURING_JOB) {
        xSemaphoreTake(s_ring_lock, portMAX_DELAY);
        int urgent = lb_has_urgent(&s_ps->store, s_ring);
        xSemaphoreGive(s_ring_lock);
        if (urgent) save_pending();
    }
    xSemaphoreGive(s_store_lock);
}

void logbook_clear(void)
{
    if (!s_ps) return;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    lb_ring_clear(s_ring);
    push_sys('S', (uint32_t)(esp_timer_get_time() / 1000), s_ps->first);
    s_errors = 0;
    s_dropped = 0;
    xSemaphoreGive(s_ring_lock);
    if (s_store_ok) lb_store_clear(&s_ps->store);
    xSemaphoreGive(s_store_lock);
}

unsigned logbook_errors(void) { return s_errors; }
int logbook_dropped(void) { return s_dropped; }

uint32_t logbook_newest(void)
{
    if (!s_ps) return 0;
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    uint32_t n = lb_ring_newest(s_ring);
    xSemaphoreGive(s_ring_lock);
    return n;
}

int logbook_read_next(lb_line_t *out)
{
    if (!s_ps) return 0;
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    int got = lb_read_next(&s_ps->reader, out);
    xSemaphoreGive(s_ring_lock);
    xSemaphoreGive(s_store_lock);
    return got;
}

static void read_open(uint32_t after, const lb_store_t *st)
{
    xSemaphoreTake(s_store_lock, portMAX_DELAY);
    xSemaphoreTake(s_ring_lock, portMAX_DELAY);
    lb_read_begin(&s_ps->reader, s_ring, st, after);
    xSemaphoreGive(s_ring_lock);
    xSemaphoreGive(s_store_lock);
}

void logbook_read_begin(uint32_t after, unsigned newest)
{
    if (!s_ps) return;

    // Decided once for the whole request. The port task holds the job lock for a moment now and
    // then with nothing printing, and the count below has to see the same lines as the read.
    const lb_store_t *st = s_store_ok && !(s_busy && s_busy()) ? &s_ps->store : NULL;
    read_open(after, st);
    if (!newest) return;

    // Numbers skip the lines flash never kept, so count rather than subtract.
    unsigned total = 0;
    while (logbook_read_next(&s_ps->skip)) total++;
    read_open(after, st);
    while (total > newest && logbook_read_next(&s_ps->skip)) total--;
}
