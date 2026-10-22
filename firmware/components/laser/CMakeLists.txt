# SPDX-License-Identifier: GPL-3.0-or-later
# lw_*.c are pure (tools/rip tests them on the host); laser.c is the board's.
idf_component_register(SRCS "lw_query.c" "lw_status.c" "lw_caps.c" "lw_core.c" "laser.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps localtalk ipp_server job_pipe ps_page ripjob halftone
                                nvs_flash esp_timer heap)
