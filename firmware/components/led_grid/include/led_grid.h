// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the SK6812 panel chain: owns the RMT strip handle and the
// render task. The pure frame renderer is led_grid_core.h.
#pragma once
#include "esp_err.h"
#include "printer_state_core.h"
#include "led_grid_core.h"

// Starts the render task over the RMT-driven SK6812 chain. ESP_OK no-op when
// CONFIG_AIRWRITER_LED_DATA_PIN < 0 (no strip on this board). Call once,
// after the IO48 boot clamp -- the RMT driver takes the pin over from there.
// The render task reads printer_state directly every 30 ms tick; there is no
// push entry point here.
esp_err_t led_grid_start(void);

// Gamer mode (DIP7): Ready and Asleep drift through RGB instead of white.
// Safe from any task; the render task picks it up on its next frame.
void led_grid_set_gamer(int on);

// Shows a task (lg_act_t, led_grid_core.h) in its family's slot: the factory
// reset, a firmware update or a TashTalk flash. A different act restarts the
// slot's clock; the same act again only updates `arg` (firmware progress), so
// its animation runs on. `arg` is RESET_CANCEL's starting fill or the
// firmware's progress, 0..1000. A family's NONE clears its slot. Safe from any
// task.
void led_grid_action(lg_act_t act, uint16_t arg);

// Shows an acknowledgement over the state from now, replacing the one showing.
// One raised while the panel is booting waits for the Spin to end, then plays
// whole. Safe from any task.
void led_grid_blip(lg_blip_t b);

// Whose job the next Printing is: set it before posting PS_EV_JOB_START.
void led_grid_set_source(lg_src_t s);
