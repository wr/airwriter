// SPDX-License-Identifier: GPL-3.0-or-later
// Plays every panel light effect through the firmware's own renderer
// (led_grid_core) and prints the frames for render.py, which draws the wiki's
// GIFs. One scene per GIF, named for the file it becomes.
//
// Output, one line each:
//   M r,c,mat ...         the chain's dots (LG_MAP), once
//   P name loop           a scene; loop is "cut" or the crossfade in ms that
//                         joins its end to its start (for the endless states)
//   37*3 numbers          one frame: 16-bit levels (8.8 fixed point), chain order
#include <stdio.h>
#include <string.h>
#include "led_grid_core.h"

#define STEP_MS 40              // 25 fps

typedef struct {
    uint32_t len;               // ms
    ps_state_t st, prev;
    uint32_t since0;            // since_ms at the segment's start
    uint32_t settle;            // BOOTING: the boot wave starts going out this far in (0: never)
    uint8_t  act; uint16_t arg; uint8_t ramp;   // ramp: arg runs 0..1000 over the segment
    uint8_t  blip;
    uint8_t  src, gamer, greeted, phone, cancelled, join_failed, cable;
    uint8_t  asleep; uint32_t asleep0;
    uint8_t  dark;              // the LEDs are off: no app running yet
} seg_t;

static uint32_t T;              // the panel's clock, continuous across a scene's segments

static void seg(seg_t g)
{
    for (uint32_t s = 0; s < g.len; s += STEP_MS, T += STEP_MS) {
        ps_view_t v;
        memset(&v, 0, sizeof v);
        v.state = g.st;
        v.prev = g.prev;
        v.since_ms = g.since0 + s;
        v.greeted = g.greeted;
        v.phone_joined = g.phone;
        v.cancelled = g.cancelled;
        v.join_failed = g.join_failed;
        v.asleep = g.asleep;
        v.asleep_ms = g.asleep ? g.asleep0 + s : 0;
        if (g.settle && v.since_ms >= g.settle) {
            v.spin_out = 1;
            v.spin_out_ms = v.since_ms - g.settle;
        }
        if (g.cable) {
            v.cable = g.cable;
            v.cable_ms = s;
        }
        lg_opts_t o;
        memset(&o, 0, sizeof o);
        o.src = g.src;
        o.gamer = g.gamer;
        if (g.act) {
            lg_slot_t *k = &o.task[lg_family(g.act)];
            k->act = g.act;
            k->ms = s;
            k->arg = g.ramp ? (uint16_t)(s * 1000 / g.len) : g.arg;
        }
        o.blip = g.blip;
        o.blip_ms = s;
        uint16_t hi[LG_NUM_LEDS][3];
        if (g.dark) memset(hi, 0, sizeof hi);
        else lg_render16(&v, &o, T, hi);
        for (int i = 0; i < LG_NUM_LEDS; i++)
            printf("%u %u %u%c", hi[i][0], hi[i][1], hi[i][2], i == LG_NUM_LEDS - 1 ? '\n' : ' ');
    }
}

// t0 picks where the ambient clocks are when the scene starts: a multiple of
// a state's period makes it begin on its own first frame.
static void scene(const char *name, const char *loop, uint32_t t0)
{
    printf("P %s %s\n", name, loop);
    T = t0;
}

#define SEG(...) seg((seg_t){__VA_ARGS__})
#define READY(ms) SEG(.len = ms, .st = PS_READY, .prev = PS_JOINING, .since0 = 60000)
#define OFFLINE(ms) SEG(.len = ms, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 60000)

int main(void)
{
    printf("M");
    for (int i = 0; i < LG_NUM_LEDS; i++)
        printf(" %d,%d,%c", LG_MAP[i].r, LG_MAP[i].c, LG_MAP[i].mat);
    printf("\n");

    // ---------------------------------------------------------- the states
    scene("boot-wave", "cut", 0);   // a board that settles early: the shortest boot, 3 s
    SEG(.len = 3000, .st = PS_BOOTING, .settle = 1700);
    SEG(.len = 600, .st = PS_BOOTING, .since0 = 3000, .settle = 1700);

    scene("setup", "cut", 104000);              // two turns of the 2.6 s arm
    SEG(.len = 5200, .st = PS_SETUP, .prev = PS_BOOTING, .since0 = 10000);

    scene("setup-phone", "800", 100000);
    SEG(.len = 7000, .st = PS_SETUP, .prev = PS_BOOTING, .since0 = 20000, .phone = 1);

    scene("joining", "cut", 99000);             // four 900 ms rings
    SEG(.len = 3600, .st = PS_JOINING, .prev = PS_BOOTING, .since0 = 5000);

    scene("wifi-connected", "cut", 100000);
    SEG(.len = 1800, .st = PS_JOINING, .prev = PS_BOOTING, .since0 = 5000);
    SEG(.len = 3200, .st = PS_READY, .prev = PS_JOINING, .greeted = 1);
    SEG(.len = 1600, .st = PS_READY, .prev = PS_JOINING, .since0 = 3200);

    scene("ready", "1000", 100000);
    READY(9000);

    scene("gamer", "1000", 100000);
    SEG(.len = 9000, .st = PS_READY, .prev = PS_JOINING, .since0 = 60000, .gamer = 1);

    scene("offline", "1500", 100000);
    OFFLINE(12000);

    scene("printing", "cut", 108000);           // one page: 8 rows of 1.5 s
    SEG(.len = 12000, .st = PS_PRINTING, .prev = PS_READY, .since0 = 2000);

    scene("printing-localtalk", "cut", 108000);
    SEG(.len = 12000, .st = PS_PRINTING, .prev = PS_READY, .since0 = 2000, .src = LG_SRC_LOCALTALK);

    scene("attention", "cut", 100000);          // two of the 2 s double ripples
    SEG(.len = 4000, .st = PS_ATTENTION, .prev = PS_PRINTING, .since0 = 40000);

    scene("failed", "cut", 100500);
    SEG(.len = 1500, .st = PS_PRINTING, .prev = PS_READY, .since0 = 2000);
    SEG(.len = 5500, .st = PS_FAILED, .prev = PS_PRINTING);

    scene("cable-pass", "cut", 100000);         // tapped with the cable in both sockets
    OFFLINE(1000);
    SEG(.len = PS_CABLE_MS, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 61000, .cable = PS_CABLE_PASS);
    SEG(.len = 1000, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 61000 + PS_CABLE_MS);

    scene("cable-fail", "cut", 100000);
    OFFLINE(1000);
    SEG(.len = PS_CABLE_MS, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 61000, .cable = PS_CABLE_FAIL);
    SEG(.len = 1000, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 61000 + PS_CABLE_MS);

    scene("cable-no-chip", "cut", 100000);      // the same tap with no LocalTalk chip (W-1063)
    OFFLINE(1200);
    SEG(.len = PS_CABLE_NO_CHIP_MS, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 61200, .cable = PS_CABLE_NO_CHIP);
    SEG(.len = 1200, .st = PS_OFFLINE, .prev = PS_READY, .since0 = 61200 + PS_CABLE_NO_CHIP_MS);

    scene("asleep", "cut", 115000);             // three breaths, each at a new dot
    SEG(.len = 3 * LG_BREATH_MS, .st = PS_READY, .prev = PS_JOINING, .since0 = 400000, .asleep = 1, .asleep0 = 60000);

    scene("falling-asleep", "cut", 100000);
    READY(1500);
    SEG(.len = 13000, .st = PS_READY, .prev = PS_JOINING, .since0 = 361500, .asleep = 1);

    // ---------------------------------------------------- moments that open a state
    scene("printer-gone", "cut", 100000);
    READY(1500);
    SEG(.len = 3500, .st = PS_OFFLINE, .prev = PS_READY);

    scene("printer-back", "cut", 100000);
    OFFLINE(1500);
    SEG(.len = 3500, .st = PS_READY, .prev = PS_OFFLINE);

    scene("page-out", "cut", 105000);          // the last two rows, then out
    SEG(.len = 3000, .st = PS_PRINTING, .prev = PS_READY, .since0 = 2000);
    SEG(.len = 2500, .st = PS_READY, .prev = PS_PRINTING);

    scene("cancelled", "cut", 106500);
    SEG(.len = 3000, .st = PS_PRINTING, .prev = PS_READY, .since0 = 2000);
    SEG(.len = 2000, .st = PS_READY, .prev = PS_PRINTING, .cancelled = 1);

    scene("join-failed", "cut", 100000);
    SEG(.len = 2000, .st = PS_JOINING, .prev = PS_BOOTING, .since0 = 50000);
    SEG(.len = 4200, .st = PS_SETUP, .prev = PS_JOINING, .join_failed = 1);

    // ---------------------------------------------------------- tasks
    scene("update-installing", "cut", 100000);  // the download sped up from about a minute
    READY(1000);
    SEG(.len = 6000, .st = PS_READY, .since0 = 61000, .act = LG_ACT_FW_DOWNLOADING, .ramp = 1);
    SEG(.len = 1600, .st = PS_READY, .since0 = 67000, .act = LG_ACT_FW_VERIFYING);

    scene("update-installed", "cut", 100000);
    SEG(.len = 800, .st = PS_READY, .since0 = 67000, .act = LG_ACT_FW_VERIFYING);
    SEG(.len = 2400, .st = PS_READY, .since0 = 67800, .act = LG_ACT_FW_VERIFIED);
    SEG(.len = 3000, .st = PS_BOOTING, .settle = 1700);

    scene("tashtalk-ok", "cut", 100000);
    READY(1000);
    SEG(.len = 2200, .st = PS_READY, .since0 = 61000, .act = LG_ACT_PIC_FLASHING);
    SEG(.len = LG_PIC_OK_MS, .st = PS_READY, .since0 = 63200, .act = LG_ACT_PIC_OK);
    READY(1000);

    scene("tashtalk-failed", "cut", 100000);
    READY(1000);
    SEG(.len = 1500, .st = PS_READY, .since0 = 61000, .act = LG_ACT_PIC_FLASHING);
    SEG(.len = LG_PIC_FAILED_MS, .st = PS_READY, .since0 = 62500, .act = LG_ACT_PIC_FAILED);
    READY(1000);

    scene("reset-cancel", "cut", 100000);       // held 4 s, then let go
    READY(1000);
    SEG(.len = 4000, .st = PS_READY, .since0 = 61000, .act = LG_ACT_RESET_HOLD);
    SEG(.len = LG_RESET_CANCEL_MS, .st = PS_READY, .since0 = 65000, .act = LG_ACT_RESET_CANCEL,
        .arg = 4000 * 1000 / LG_RESET_FILL_MS);
    READY(1500);

    scene("reset", "cut", 100000);              // held to the end: erased, restarts into Setup
    READY(1000);
    SEG(.len = LG_RESET_FILL_MS, .st = PS_READY, .since0 = 61000, .act = LG_ACT_RESET_HOLD);
    SEG(.len = 2600, .st = PS_READY, .since0 = 68000, .act = LG_ACT_RESET_DONE);
    SEG(.len = 3000, .st = PS_BOOTING, .settle = 1700);
    SEG(.len = 2600, .st = PS_SETUP, .prev = PS_BOOTING);

    scene("reset-power-up", "cut", 0);          // held through power-up: the bootloader erased it
    SEG(.len = 1500, .dark = 1);
    SEG(.len = 3000, .st = PS_BOOTING, .settle = 1700);
    SEG(.len = LG_RESET_DONE_MS, .st = PS_SETUP, .prev = PS_BOOTING, .act = LG_ACT_RESET_CONFIRM);
    SEG(.len = 2600, .st = PS_SETUP, .prev = PS_BOOTING, .since0 = LG_RESET_DONE_MS);

    // ---------------------------------------------------------- acknowledgements
    scene("localtalk-on", "cut", 100000);
    READY(1200);
    SEG(.len = LG_BLIP_LT_ON_MS, .st = PS_READY, .since0 = 61200, .blip = LG_BLIP_LT_ON);
    READY(1200);

    scene("mac-found", "cut", 100000);
    READY(1200);
    SEG(.len = LG_BLIP_MAC_MS, .st = PS_READY, .since0 = 61200, .blip = LG_BLIP_MAC);
    READY(1200);

    scene("busy", "cut", 102000);
    SEG(.len = 1500, .st = PS_PRINTING, .prev = PS_READY, .since0 = 2000);
    SEG(.len = LG_BLIP_BUSY_MS, .st = PS_PRINTING, .prev = PS_READY, .since0 = 3500, .blip = LG_BLIP_BUSY);
    SEG(.len = 1500, .st = PS_PRINTING, .prev = PS_READY, .since0 = 4100);

    scene("saved", "cut", 100000);
    READY(1200);
    SEG(.len = LG_BLIP_SAVED_MS, .st = PS_READY, .since0 = 61200, .blip = LG_BLIP_SAVED);
    READY(1500);
    return 0;
}
