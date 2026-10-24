# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ps_page.c" "ps_testpage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ripjob halftone)
