// SPDX-License-Identifier: GPL-3.0-or-later
// The panel chain. Rendering is the pure core; this task owns time and the
// wire. On a devkit with no strip (pin -1) start() is a successful no-op, so
// main never has to care. The IO48 boot clamp hands the pin over to the RMT
// driver here -- by the first frame the line is driven, never floating.
#include "led_grid.h"
#include "led_grid_core.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "printer_state.h"
#include <string.h>

#define LG_RMT_SYMBOLS 1024    // the DMA buffer: one frame of 37 LEDs is 888

static led_strip_handle_t s_strip;
static int s_count;
static volatile uint8_t s_gamer;   // DIP7, set from dip_config's task

// The tasks and the acknowledgement over the state, set from other tasks
// (led_grid_action, led_grid_blip): what each shows and when it began.
// The clocks are 64-bit milliseconds (lg_elapsed): a 32-bit difference goes
// negative after 24.8 days and would freeze a finished one-shot on its first
// frame.
typedef struct { uint8_t act; uint16_t arg; uint64_t t0; } slot_t;
static portMUX_TYPE s_act_mux = portMUX_INITIALIZER_UNLOCKED;
static slot_t   s_task[LG_FAMS];
static uint8_t  s_blip;
static uint64_t s_blip_t0;
static volatile uint8_t s_src;     // lg_src_t, whose job Printing is

// The frame being driven, for the settings page's picture of the panel
// (led_grid_frame). Copied every frame: 222 bytes, well under a microsecond.
static portMUX_TYPE s_frame_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_frame[LG_NUM_LEDS][3];
static volatile uint8_t s_running;

static uint64_t now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

// Core 1, away from Wi-Fi: the Wi-Fi task is pinned to core 0, and on board 1
// the WPA3 handshake kept core 0 busy from 2.7 s to 3.8 s after power-on.
// Unpinned, this task ran every frame on core 0 there, and frames came up to
// 316 ms apart: the Spin stuttered (W-1060 frame log). The strip is created on
// this task too, so its RMT interrupt is allocated on core 1 as well.
#define LG_CORE (portNUM_PROCESSORS > 1 ? 1 : 0)

static SemaphoreHandle_t s_started;
static esp_err_t s_start_err;

static esp_err_t strip_start(void);

static void lg_task(void *arg)
{
    (void)arg;
    s_start_err = strip_start();
    xSemaphoreGive(s_started);
    if (s_start_err != ESP_OK) vTaskDelete(NULL);
    s_running = 1;
    // Rendered at 8.8 fixed point and temporally dithered down to 8 bits, at
    // a steady 100 fps: at a few counts of drive an 8-bit breathe has only a
    // handful of levels and visibly stutters; the dither carries each
    // channel's fraction into the next frame, and at 100 fps it blends.
    static uint16_t hi[LG_NUM_LEDS][3];
    static lg_dither_t dith;
    uint8_t frame[LG_NUM_LEDS][3];
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        // Reads printer_state directly each frame rather than being pushed a
        // state, so two racing posters can never deliver an update to this
        // task out of order -- the next frame just re-reads whatever is
        // current.
        ps_view_t v = printer_state_view();
        uint64_t now = now_ms();
        lg_opts_t o = { .gamer = s_gamer, .src = s_src };
        portENTER_CRITICAL(&s_act_mux);
        for (int f = 0; f < LG_FAMS; f++)
            o.task[f] = (lg_slot_t){ s_task[f].act, s_task[f].arg, lg_elapsed(now, s_task[f].t0) };
        // An acknowledgement raised during the Spin waits it out: its clock
        // starts again every frame until the panel has booted.
        if (v.state == PS_BOOTING) s_blip_t0 = now;
        o.blip = s_blip;
        o.blip_ms = lg_elapsed(now, s_blip_t0);
        portEXIT_CRITICAL(&s_act_mux);
        lg_render16(&v, &o, (uint32_t)now, hi);    // animation phase: wraps harmlessly
        portENTER_CRITICAL(&s_frame_mux);
        memcpy(s_frame, hi, sizeof s_frame);
        portEXIT_CRITICAL(&s_frame_mux);
        lg_dither(&dith, hi, frame);
        for (int i = 0; i < s_count && i < LG_NUM_LEDS; i++)
            led_strip_set_pixel(s_strip, (uint32_t)i,
                                 frame[i][0], frame[i][1], frame[i][2]);
        led_strip_refresh(s_strip);
        xTaskDelayUntil(&last, pdMS_TO_TICKS(10));
    }
}

static esp_err_t strip_start(void)
{
    s_count = CONFIG_AIRWRITER_LED_COUNT;
    led_strip_config_t sc = {
        .strip_gpio_num = CONFIG_AIRWRITER_LED_DATA_PIN,
        .max_leds = (uint32_t)s_count,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    // The whole frame (24 bits a LED) goes out of one DMA buffer. With the
    // default 48-symbol RMT block an interrupt has to refill it every ~30 us,
    // and that interrupt is not cache-safe: during a Wi-Fi join (the scan, the
    // handshake, NVS writes with the cache off) a late refill sent stale bits
    // and the Spin stuttered (W-1049 bench). The S3 has one DMA TX channel.
    _Static_assert(CONFIG_AIRWRITER_LED_COUNT * 24 < LG_RMT_SYMBOLS, "frame must fit the DMA buffer");
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000,
                                  .mem_block_symbols = LG_RMT_SYMBOLS, .flags.with_dma = true };
    esp_err_t err = led_strip_new_rmt_device(&sc, &rc, &s_strip);
    if (err != ESP_OK) return err;
    return led_strip_clear(s_strip);
}

esp_err_t led_grid_start(void)
{
    if (CONFIG_AIRWRITER_LED_DATA_PIN < 0) return ESP_OK;
    // Waits for the task to have the strip, so the RMT driver owns IO48 by
    // the time this returns, as the boot clamp in main expects.
    s_started = xSemaphoreCreateBinary();
    if (!s_started) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(lg_task, "led_grid", 3072, NULL, 3, NULL, LG_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_started, portMAX_DELAY);
    return s_start_err;
}

int led_grid_frame(uint16_t out[LG_NUM_LEDS][3])
{
    if (!s_running) return 0;
    portENTER_CRITICAL(&s_frame_mux);
    memcpy(out, s_frame, sizeof s_frame);
    portEXIT_CRITICAL(&s_frame_mux);
    return 1;
}

void led_grid_set_gamer(int on)
{
    s_gamer = on ? 1 : 0;
}

void led_grid_action(lg_act_t act, uint16_t arg)
{
    uint64_t now = now_ms();
    slot_t *s = &s_task[lg_family((uint8_t)act)];
    portENTER_CRITICAL(&s_act_mux);
    if (s->act != (uint8_t)act) {
        s->act = (uint8_t)act;
        s->t0 = now;
    }
    s->arg = arg;
    portEXIT_CRITICAL(&s_act_mux);
}

void led_grid_blip(lg_blip_t b)
{
    uint64_t now = now_ms();
    portENTER_CRITICAL(&s_act_mux);
    s_blip = (uint8_t)b;
    s_blip_t0 = now;
    portEXIT_CRITICAL(&s_act_mux);
}

void led_grid_set_source(lg_src_t s)
{
    s_src = (uint8_t)s;
}
