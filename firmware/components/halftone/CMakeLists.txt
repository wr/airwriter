# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "halftone.c" "apple_tables.c" "apple_tables_lw.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bigmem)
