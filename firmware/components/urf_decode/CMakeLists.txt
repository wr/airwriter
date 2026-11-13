# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "urf_decode.c"
                       INCLUDE_DIRS "include"
                       REQUIRES bigmem)
# W-1143: the rip's per-row code is built for speed rather than ESP-IDF's debug optimisation,
# as tools/rip builds it for the host tests.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
