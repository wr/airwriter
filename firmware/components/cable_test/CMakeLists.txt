# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "cable_core.c" "cable_test.c"
                       INCLUDE_DIRS "include"
                       REQUIRES localtalk serial_out esp_driver_rmt esp_driver_uart esp_driver_gpio
                                esp_hw_support esp_timer)
