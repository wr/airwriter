# SPDX-License-Identifier: GPL-3.0-or-later
# W-1102: QuickDraw LaserWriters on the printer port.
idf_component_register(SRCS "qd_model.c" "qd_wire.c" "qd_status.c" "qd_page.c" "qd_session.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps halftone ripjob stylewriter bigmem)
