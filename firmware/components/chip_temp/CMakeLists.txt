# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "chip_temp.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_tsens)
