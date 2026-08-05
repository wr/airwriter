# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES urf_decode resample halftone imagewriter ripjob job_pipe serial_out ipp_server esp_wifi nvs_flash esp_netif)
