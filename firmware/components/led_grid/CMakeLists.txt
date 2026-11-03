# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "led_grid.c" "led_grid_core.c" "led_map.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_state esp_timer esp_driver_gptimer led_strip)
