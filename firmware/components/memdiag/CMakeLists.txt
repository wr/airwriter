# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "memdiag.c"
                       INCLUDE_DIRS "include"
                       REQUIRES heap esp_timer esp_hw_support)
