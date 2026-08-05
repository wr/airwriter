# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "net_provision.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi esp_netif lwip)
