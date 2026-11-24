// SPDX-License-Identifier: GPL-3.0-or-later
#include "ports.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "port_detect_core.h"
#include "serial_out.h"

static const char *TAG = "ports";

static port_sock_t s_sock[PORTS_MAX];
static port_job_t  s_job[PORTS_MAX];
static int s_count, s_either;

#ifndef CONFIG_AIRWRITER_J4_UART_NUM
#define CONFIG_AIRWRITER_J4_UART_NUM 0
#endif
#ifndef CONFIG_AIRWRITER_J2_SENSE_PIN
#define CONFIG_AIRWRITER_J2_SENSE_PIN -1
#endif
#ifndef CONFIG_AIRWRITER_J4_SENSE_PIN
#define CONFIG_AIRWRITER_J4_SENSE_PIN -1
#endif

static void table(int lc)
{
    // J2: the printer port everywhere.
    s_sock[0] = (port_sock_t){
        .uart = CONFIG_AIRWRITER_UART_NUM, .tx = CONFIG_AIRWRITER_TX_PIN,
        .rx = CONFIG_AIRWRITER_RX_PIN, .de = CONFIG_AIRWRITER_J2_DE_PIN,
        .hski = CONFIG_AIRWRITER_CTS_PIN, .hsko_drv = CONFIG_AIRWRITER_HSKI_DRV_PIN,
        .pic = { CONFIG_AIRWRITER_J2_PIC_MCLR_PIN, CONFIG_AIRWRITER_J2_PIC_CLK_PIN,
                 CONFIG_AIRWRITER_J2_PIC_DAT_PIN, 0 },
        .lt_cts = CONFIG_AIRWRITER_J2_LT_CTS_PIN, .sense = CONFIG_AIRWRITER_J2_SENSE_PIN,
        .can_print = 1,
    };
    // J4: rev A's AppleTalk port; rev 2026.11's second socket, with a printer line too.
    s_sock[1] = (port_sock_t){
        .uart = CONFIG_AIRWRITER_J4_UART_NUM, .tx = CONFIG_AIRWRITER_J4_TX_PIN,
        .rx = CONFIG_AIRWRITER_J4_RX_PIN, .de = CONFIG_AIRWRITER_J4_DE_PIN,
        .hski = CONFIG_AIRWRITER_J4_HSKI_PIN, .hsko_drv = CONFIG_AIRWRITER_J4_HSKO_DRV_PIN,
        .pic = pic_prog_u7(), .lt_cts = CONFIG_AIRWRITER_LT_CTS_PIN,
        .sense = CONFIG_AIRWRITER_J4_SENSE_PIN,
    };
    s_sock[1].can_lt = s_sock[1].pic.clk >= 0 && s_sock[1].pic.dat >= 0;
    s_sock[1].can_print = s_sock[1].tx >= 0 && s_sock[1].rx >= 0 && s_sock[1].de >= 0;
    s_sock[0].can_lt = s_sock[0].pic.mclr >= 0 && s_sock[0].pic.clk >= 0 &&
                       s_sock[0].pic.dat >= 0;
    s_count = 2;
    // The LC: J2 alone, U10 and the sense parts not fitted.
    if (lc) {
        s_count = 1;
        s_sock[0].can_lt = 0;
        s_sock[0].sense = s_sock[1].sense = -1;
    }
    s_either = s_count == 2 && s_sock[0].can_lt && s_sock[1].can_print;
}

static void input(int pin, gpio_pull_mode_t pull)
{
    if (pin < 0) return;
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, pull);
}

#ifdef CONFIG_AIRWRITER_INVERT_TX
#define TX_IDLE 0
#else
#define TX_IDLE 1
#endif

static void output(int pin, int level)
{
    if (pin < 0) return;
    gpio_set_level(pin, level);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
}

// The pins alone; serial_out's part is the caller's when start is 0 (at boot, before the
// line exists).
static void hand_over(int i, port_job_t job, int start)
{
    const port_sock_t *s = &s_sock[i];
    if (job == PORT_JOB_PRINTER) {
        // The PIC into reset before anything drives its lines: then its outputs are off.
        if (s->pic.mclr >= 0) {
            gpio_reset_pin(s->pic.mclr);
            gpio_set_pull_mode(s->pic.mclr, GPIO_FLOATING);
            output(s->pic.mclr, 0);
        }
        // With the PIC in reset nothing drives these, and a PIC in reset listens there for
        // the LVP key: pulled down.
        if (s->pic.mclr >= 0) {
            input(s->pic.clk, GPIO_PULLDOWN_ONLY);
            input(s->pic.dat, GPIO_PULLDOWN_ONLY);
            input(s->lt_cts, GPIO_PULLDOWN_ONLY);
        }
        // The ESP32's TX takes the driver's input, idle, before the driver goes on, so nothing
        // floats onto the line (W-1176's order): the UART's at a change, the pin's own at boot,
        // before the UART exists.
        if (start) serial_out_park(i, 0);
        else if (s->tx >= 0) {
            gpio_reset_pin(s->tx);
            output(s->tx, TX_IDLE);
        }
        if (s->de >= 0) {
            gpio_reset_pin(s->de);
            gpio_set_pull_mode(s->de, GPIO_FLOATING);
            output(s->de, 1);
        }
    } else {
        if (start) serial_out_park(i, 1);
        else {
            input(s->tx, GPIO_FLOATING);
            input(s->rx, GPIO_FLOATING);
        }
        input(s->de, GPIO_FLOATING);
        // MCLR back to its reset state: the PIC runs. Its UART pins are LocalTalk's to take.
        if (s->pic.mclr >= 0) gpio_reset_pin(s->pic.mclr);
    }
    s_job[i] = job;
}

static int can(int i, port_job_t job)
{
    return job == PORT_JOB_PRINTER ? s_sock[i].can_print
         : job == PORT_JOB_LOCALTALK ? s_sock[i].can_lt : 0;
}

void ports_init(int lc, const port_job_t jobs[PORTS_MAX])
{
    table(lc);
    for (int i = 0; i < s_count; i++) {
        port_job_t j = jobs ? jobs[i] : PORT_JOB_NONE;
        if (!can(i, j)) j = s_sock[i].can_print ? PORT_JOB_PRINTER : PORT_JOB_LOCALTALK;
        if (!can(i, j)) j = PORT_JOB_NONE;
        if (s_either) hand_over(i, j, 0);
        else s_job[i] = j;      // rev A, the LC: fixed, and set up as before W-1179
    }
    for (int i = 0; i < s_count; i++)
        ESP_LOGI(TAG, "port %d (J%d): %s", i + 1, i ? 4 : 2, ports_job_name(s_job[i]));
}

int ports_count(void) { return s_count; }
int ports_either(void) { return s_either; }

const port_sock_t *ports_sock(int i)
{
    return i >= 0 && i < s_count ? &s_sock[i] : NULL;
}

port_job_t ports_job(int i)
{
    return i >= 0 && i < s_count ? s_job[i] : PORT_JOB_NONE;
}

int ports_with(port_job_t job)
{
    for (int i = 0; i < s_count; i++)
        if (s_job[i] == job) return i;
    return -1;
}

void ports_hw(int i, port_job_t job)
{
    if (i < 0 || i >= s_count || !s_either || !can(i, job) || s_job[i] == job) return;
    ESP_LOGI(TAG, "port %d (J%d): %s, was %s", i + 1, i ? 4 : 2, ports_job_name(job),
             ports_job_name(s_job[i]));
    hand_over(i, job, 1);
}

const char *ports_job_name(port_job_t j)
{
    return j == PORT_JOB_PRINTER ? "printer" : j == PORT_JOB_LOCALTALK ? "localtalk" : "none";
}

// ------------------------------------------------------------------ W-1185: the port sense
// Each socket's sense line through 4.7 k to its GPIO, open-collector, so the pin's pull-up
// holds it high. A LocalTalk frame pulses it at up to 230.4 kbit/s, faster than any look:
// PCNT counts its rising edges between looks (a single fall shows in the level), so a look
// that saw none knows the line held all window. 32767 rising edges are 140 ms of frames; the
// unit accumulates past that, so a late look still counts true.
#define SENSE_LIMIT  32767

static pcnt_unit_handle_t s_sense_pcnt[PORTS_MAX];
static pd_sense_t         s_sense[PORTS_MAX];
static volatile uint8_t   s_sense_held[PORTS_MAX];
static uint32_t         (*s_sense_tx_at)(int sock);
static esp_timer_handle_t s_sense_timer;

static pcnt_unit_handle_t sense_counter(int pin)
{
    pcnt_unit_handle_t u = NULL;
    pcnt_channel_handle_t ch = NULL;
    pcnt_unit_config_t uc = { .low_limit = -1, .high_limit = SENSE_LIMIT, .flags.accum_count = 1 };
    pcnt_chan_config_t cc = { .edge_gpio_num = pin, .level_gpio_num = -1 };
    // Glitches under 1 us are noise: a frame's half cells are 2.2 us.
    pcnt_glitch_filter_config_t gf = { .max_glitch_ns = 1000 };
    if (pcnt_new_unit(&uc, &u) != ESP_OK) return NULL;
    if (pcnt_new_channel(u, &cc, &ch) != ESP_OK ||
        pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_HOLD) != ESP_OK ||
        pcnt_unit_set_glitch_filter(u, &gf) != ESP_OK ||
        pcnt_unit_add_watch_point(u, SENSE_LIMIT) != ESP_OK ||
        pcnt_unit_enable(u) != ESP_OK || pcnt_unit_start(u) != ESP_OK) {
        if (ch) pcnt_del_channel(ch);
        pcnt_del_unit(u);
        return NULL;
    }
    return u;
}

// On the esp_timer task, every PD_SENSE_LOOK_MS: cheap, and nothing logged here.
static void sense_look(void *arg)
{
    (void)arg;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < s_count; i++) {
        if (!s_sense_pcnt[i]) continue;
        int n = 0;
        pcnt_unit_get_count(s_sense_pcnt[i], &n);
        pcnt_unit_clear_count(s_sense_pcnt[i]);
        int level = gpio_get_level(s_sock[i].sense);
        uint32_t tx = s_sense_tx_at ? s_sense_tx_at(i) : 0;
        s_sense_held[i] = pd_sense_look(&s_sense[i], level, n != 0, tx, now);
    }
}

void ports_sense_start(uint32_t (*tx_at)(int sock))
{
    if (!s_either || s_sense_timer) return;
    s_sense_tx_at = tx_at;
    int any = 0;
    for (int i = 0; i < s_count; i++) {
        int pin = s_sock[i].sense;
        if (pin < 0) continue;
        // As app_main's first lines left it. gpio_config selects the GPIO function: IO39 leaves
        // reset as JTAG's MTCK.
        gpio_config_t io = { .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE };
        gpio_config(&io);
        s_sense_pcnt[i] = sense_counter(pin);
        if (!s_sense_pcnt[i]) {
            ESP_LOGW(TAG, "port %d: no edge counter for its sense; it listens first", i + 1);
            continue;
        }
        any = 1;
    }
    if (!any) return;
    const esp_timer_create_args_t ta = { .callback = sense_look, .name = "port_sense" };
    if (esp_timer_create(&ta, &s_sense_timer) != ESP_OK ||
        esp_timer_start_periodic(s_sense_timer, PD_SENSE_LOOK_MS * 1000ULL) != ESP_OK) {
        ESP_LOGW(TAG, "no timer for the port sense; the ports listen first");
        for (int i = 0; i < s_count; i++) s_sense_pcnt[i] = NULL;
    }
}

uint8_t ports_sense(int i)
{
    return i >= 0 && i < s_count ? s_sense_held[i] : PD_SENSE_NONE;
}
