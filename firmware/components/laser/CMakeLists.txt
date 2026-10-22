# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "lw_query.c" "lw_status.c" "lw_caps.c" "lw_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps localtalk)
