// SPDX-License-Identifier: GPL-3.0-or-later
// ESP glue for the SK6812 panel chain: owns the RMT strip handle and the
// render task. The pure frame renderer is led_grid_core.h.
#pragma once
#include "esp_err.h"
#include "printer_state_core.h"

// Starts the render task over the RMT-driven SK6812 chain. ESP_OK no-op when
// CONFIG_AIRWRITER_LED_DATA_PIN < 0 (no strip on this board). Call once,
// after the IO48 boot clamp -- the RMT driver takes the pin over from there.
esp_err_t led_grid_start(void);

// Thread-safe: sets the state the next frame renders. Takes effect on the
// task's next 30 ms tick, not immediately.
void led_grid_set_state(ps_state_t st);
