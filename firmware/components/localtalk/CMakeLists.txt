# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "localtalk.c" "lt_node.c" "lt_proto.c" "lt_tashtalk.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_driver_gpio esp_timer nvs_flash esp_hw_support heap)
