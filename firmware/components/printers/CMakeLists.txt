# SPDX-License-Identifier: GPL-3.0-or-later
# printers_core.c and port_policy.c are pure (tools/rip tests them on the host); printers.c
# is the board's.
# stylewriter is for sw_model.h's names and caps, which are pure too.
idf_component_register(SRCS "printers_core.c" "port_policy.c" "printers.c"
                       INCLUDE_DIRS "include"
                       REQUIRES laser localtalk ipp_server printer_caps imagewriter stylewriter nvs_flash heap)
