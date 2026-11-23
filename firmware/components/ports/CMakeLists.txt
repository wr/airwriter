# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ports.c" "port_detect_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio serial_out pic_prog)
