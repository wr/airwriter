# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "test_button.c" "test_button_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio esp_timer)
