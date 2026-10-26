# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "sw_model.c" "sw_code.c" "sw_page.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps halftone ripjob bigmem)
