# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ipp_req.c" "ipp_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ipp_attrs esp_http_server esp_wifi esp_timer esp_hw_support mdns)
