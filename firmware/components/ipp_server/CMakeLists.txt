# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ipp_req.c" "ipp_job.c" "ipp_http.c" "ipp_server.c"
                       INCLUDE_DIRS "include"
                       EMBED_FILES "icons/icon-48.png" "icons/icon-128.png"
                                   "icons/icon-512.png"
                                   "icons/laser-48.png" "icons/laser-128.png"
                                   "icons/laser-512.png"
                       REQUIRES ipp_attrs esp_timer esp_hw_support lwip mdns)
