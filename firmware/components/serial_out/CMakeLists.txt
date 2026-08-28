# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "serial_out.c" "serial_writer.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_driver_gpio)
