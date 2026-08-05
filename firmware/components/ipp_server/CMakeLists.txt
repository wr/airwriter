# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ipp_req.c" "ipp_http.c" "ipp_server.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ipp_attrs esp_timer esp_hw_support lwip mdns)
