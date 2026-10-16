// SPDX-License-Identifier: GPL-3.0-or-later
// Pure frame renderer for the 37-dot art panel: (view, options, t) -> one RGB
// frame. No FreeRTOS, no I/O -- led_grid.c drives the RMT strip from its output.
#pragma once
#include <stdint.h>
#include "led_map.h"
#include "printer_state_core.h"

#define LG_BRIGHTNESS_CAP 48    // of 255, FR4 dots -- no hardware current limit on rev A
#define LG_MASK_CAP       192   // mask dots in the all-dot states: 4x, they pass ~10% / ~5%
#define LG_ALERT_CAP      96    // FR4 dots in Needs attention: it has to read across the room
#define LG_BOOT_MS        3000  // the shortest boot spin; main sets printer_state's boot_ms to this
#define LG_SPIN_OUT_MS    1300  // the Spin going out once the board settles (boot_out_ms)

// Tasks: what the board is busy doing for a person, shown over whatever state
// the panel is in, sleep included (W-1045's factory reset; W-1049 adds the
// firmware update and the TashTalk flash). Each family has its own slot in
// lg_opts_t; the first running one in family order owns the panel, so letting
// go of a reset hold uncovers an update still in progress. The enum's order is
// the family boundary (lg_family).
typedef enum {
    LG_ACT_NONE,
    LG_ACT_RESET_HOLD,      // test button held: a sweep fills the panel, orange to red
    LG_ACT_RESET_CANCEL,    // let go during the countdown: the sweep winds back
    LG_ACT_RESET_DONE,      // past the point of no return: flash, drain, two blinks, dark
                            // until the board restarts
    LG_ACT_RESET_CONFIRM,   // the same at boot, after the bootloader's reset; then the state
    LG_ACT_RESET_FAILED,    // the erase failed: Job failed's red glitch, then the state

    // A firmware update (W-1048, POST /update). Call again with the same act
    // to move its progress on; the animation keeps its clock.
    LG_ACT_FW_NONE,         // clear: the update was dropped with nothing to show
    LG_ACT_FW_DOWNLOADING,  // arg: progress 0..1000, or LG_PROGRESS_NONE
    LG_ACT_FW_INSTALLING,   // arg: progress, for a flow that writes flash after the download
    LG_ACT_FW_VERIFYING,    // the image is in; its signature is being checked
    LG_ACT_FW_VERIFIED,     // installed and set to boot: a done moment, dark until the restart
    LG_ACT_FW_REFUSED,      // bad signature, wrong image or a write error. Held back
                            // (W-1049): draws nothing, the state shows
    LG_ACT_FW_ROLLED_BACK,  // at boot: the last update was rolled back. Held back too

    // TashTalk into the PIC (POST /api/localtalk/flash).
    LG_ACT_PIC_NONE,
    LG_ACT_PIC_FLASHING,    // the six stripes with a white bar sweeping across, until replaced
    LG_ACT_PIC_OK,          // a green ripple out of the LocalTalk socket, then the state
    LG_ACT_PIC_FAILED,      // Job failed's red glitch for 3 s, then the state
} lg_act_t;

typedef enum { LG_FAM_RESET, LG_FAM_FW, LG_FAM_PIC, LG_FAMS } lg_fam_t;   // priority order
lg_fam_t lg_family(uint8_t act);

// Acknowledgements: a moment drawn over the state, never over a task, and not
// in Booting, Needs attention or Job failed (an alert is never hidden).
typedef enum {
    LG_BLIP_NONE,
    LG_BLIP_MAC,            // a Mac heard on LocalTalk: an arc of stripes from its socket
    LG_BLIP_LT_ON,          // the LocalTalk node has its ID: the stripes rise from its socket
    LG_BLIP_BUSY,           // the test button tapped during another job: two orange rings
    LG_BLIP_SAVED,          // settings saved: a white ring closes into the test button
} lg_blip_t;
int lg_blip_over(uint8_t blip, uint32_t ms);

// Milliseconds from t0 to now for a slot's clock: 0 when t0 is after now (set
// while now was being read), saturating at UINT32_MAX so a one-shot set long
// ago stays over. 64-bit in: a 32-bit difference goes negative after 24.8 days.
uint32_t lg_elapsed(uint64_t now_ms, uint64_t t0_ms);

// Whose job Printing shows: the ink is warm for AirPrint and the test page,
// the six stripes for a LocalTalk (PAP) job.
typedef enum { LG_SRC_AIRPRINT, LG_SRC_LOCALTALK } lg_src_t;

#define LG_PROGRESS_NONE    0xFFFF  // FW_DOWNLOADING / INSTALLING: length unknown

#define LG_RESET_FILL_MS    7000    // the countdown; main checks it is BTN_FILL_MS
#define LG_RESET_CANCEL_MS  400
#define LG_RESET_DONE_MS    2000    // CONFIRM ends here; DONE stays dark after it
#define LG_RESET_FAILED_MS  3000
#define LG_FW_VERIFIED_MS   2000    // then dark until the restart
#define LG_PIC_OK_MS        1600
#define LG_PIC_FAILED_MS    3000

// The moments that open a state, from ps_view_t.prev (W-1049).
#define LG_GONE_MS          2000    // Ready -> Printer offline: the white drains to the printer
#define LG_BACK_MS          2000    // Offline -> Ready, Attention -> Printing: a green wave
#define LG_PAGE_MS          1000    // Printing -> Ready, finished: the page feeds out
#define LG_CANCEL_MS         900    // Printing -> Ready, cancelled: the print stops and fades
#define LG_JOINFAIL_MS      1600    // Joining -> Setup: two red pulses, then Setup

#define LG_BLIP_MAC_MS      1100
#define LG_BLIP_LT_ON_MS    1600
#define LG_BLIP_BUSY_MS      600
#define LG_BLIP_SAVED_MS     900

typedef struct {
    uint8_t  act;               // lg_act_t, LG_ACT_NONE when the slot is empty
    uint16_t arg;               // RESET_CANCEL: the fill it winds back from, 0..1000;
                                // FW_DOWNLOADING / INSTALLING: progress
    uint32_t ms;                // time since the action began
} lg_slot_t;

typedef struct {
    uint8_t   gamer;            // DIP7: Ready and Asleep drift through RGB instead of white
    uint8_t   src;              // lg_src_t: whose job Printing is
    lg_slot_t task[LG_FAMS];    // one per family, in priority order
    uint8_t   blip;             // lg_blip_t
    uint32_t  blip_ms;          // time since the acknowledgement began
} lg_opts_t;

// Whether a one-shot action has run its course and the state shows again.
int  lg_act_over(uint8_t act, uint32_t act_ms);

void lg_render(const ps_view_t *v, const lg_opts_t *o, uint32_t t_ms,
               uint8_t out[LG_NUM_LEDS][3]);
int  lg_pilot_index(void);      // the FR4 dot right of the test button (row 3, col 4)

// Asleep: one breath every LG_BREATH_MS, each at a new FR4 dot. The dot of
// breath k (t_ms / LG_BREATH_MS): every FR4 dot takes a turn, in a shuffled
// order, before any repeats, and never the same dot twice running.
#define LG_BREATH_MS 5000
int  lg_sleep_dot(uint32_t breath);

// The same frame at 8.8 fixed point (value x 256). Slow fades at a few counts
// of drive need the fraction: an 8-bit Ready breath has only ~6 levels, and
// each step is a visible jump. lg_render() is this, rounded.
void lg_render16(const ps_view_t *v, const lg_opts_t *o, uint32_t t_ms,
                 uint16_t out[LG_NUM_LEDS][3]);

// Sigma-delta down to the LEDs' 8 bits: each channel carries its rounding
// error into the next frame, so over time the output averages to the 16-bit
// level. Zero-initialise once; call once per frame.
typedef struct { uint8_t err[LG_NUM_LEDS][3]; } lg_dither_t;
void lg_dither(lg_dither_t *d, const uint16_t in[LG_NUM_LEDS][3],
               uint8_t out[LG_NUM_LEDS][3]);
