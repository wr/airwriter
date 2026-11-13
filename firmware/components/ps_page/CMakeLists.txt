# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ps_page.c" "ps_testpage.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ripjob halftone)
# W-1143: the rip's per-row code is built for speed rather than ESP-IDF's debug optimisation,
# as tools/rip builds it for the host tests.
set_source_files_properties(ps_page.c PROPERTIES COMPILE_OPTIONS -O2)
