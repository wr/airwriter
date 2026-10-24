# SPDX-License-Identifier: GPL-3.0-or-later
# printers_core.c is pure (tools/rip tests it on the host); printers.c is the board's.
idf_component_register(SRCS "printers_core.c" "printers.c"
                       INCLUDE_DIRS "include"
                       REQUIRES laser localtalk ipp_server printer_caps imagewriter nvs_flash heap)
