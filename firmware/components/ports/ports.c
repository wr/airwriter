// SPDX-License-Identifier: GPL-3.0-or-later
#include "ports.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "serial_out.h"

static const char *TAG = "ports";

static port_sock_t s_sock[PORTS_MAX];
static port_job_t  s_job[PORTS_MAX];
static int s_count, s_either;

#ifndef CONFIG_AIRWRITER_J4_UART_NUM
#define CONFIG_AIRWRITER_J4_UART_NUM 0
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
        .lt_cts = CONFIG_AIRWRITER_J2_LT_CTS_PIN, .can_print = 1,
    };
    // J4: rev A's AppleTalk port; rev 2026.11's second socket, with a printer line too.
    s_sock[1] = (port_sock_t){
        .uart = CONFIG_AIRWRITER_J4_UART_NUM, .tx = CONFIG_AIRWRITER_J4_TX_PIN,
        .rx = CONFIG_AIRWRITER_J4_RX_PIN, .de = CONFIG_AIRWRITER_J4_DE_PIN,
        .hski = CONFIG_AIRWRITER_J4_HSKI_PIN, .hsko_drv = CONFIG_AIRWRITER_J4_HSKO_DRV_PIN,
        .pic = pic_prog_u7(), .lt_cts = CONFIG_AIRWRITER_LT_CTS_PIN,
    };
    s_sock[1].can_lt = s_sock[1].pic.clk >= 0 && s_sock[1].pic.dat >= 0;
    s_sock[1].can_print = s_sock[1].tx >= 0 && s_sock[1].rx >= 0 && s_sock[1].de >= 0;
    s_sock[0].can_lt = s_sock[0].pic.mclr >= 0 && s_sock[0].pic.clk >= 0 &&
                       s_sock[0].pic.dat >= 0;
    s_count = 2;
    // The LC: J2 alone, U10 not fitted.
    if (lc) {
        s_count = 1;
        s_sock[0].can_lt = 0;
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
        if (s->de >= 0) {
            gpio_reset_pin(s->de);
            gpio_set_pull_mode(s->de, GPIO_FLOATING);
            output(s->de, 1);
        }
        if (start) serial_out_park(i, 0);
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
    // Never two LocalTalk sockets: one node, one TashTalk UART.
    if (s_either && s_job[0] == PORT_JOB_LOCALTALK && s_job[1] == PORT_JOB_LOCALTALK)
        hand_over(1, PORT_JOB_PRINTER, 0);
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
