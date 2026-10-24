# SPDX-License-Identifier: GPL-3.0-or-later
# printers_core.c is pure (tools/rip tests it on the host).
idf_component_register(SRCS "printers_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES laser localtalk)
