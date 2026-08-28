# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "dip_config.c" "dip_config_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio)
