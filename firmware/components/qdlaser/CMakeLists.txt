# SPDX-License-Identifier: GPL-3.0-or-later
# W-1102: QuickDraw LaserWriters on the printer port.
idf_component_register(SRCS "qd_model.c" "qd_wire.c" "qd_status.c" "qd_page.c" "qd_session.c" "qd_testpage.c"
                            "qd_link.c" "qdlaser.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps halftone ripjob stylewriter bigmem serial_out job_pipe
                                esp_timer esp_rom esp_driver_gpio esp_driver_spi esp_driver_uart
                                esp_hw_support hal soc)
# W-1143: the rip's per-row code is built for speed rather than ESP-IDF's debug optimisation,
# as tools/rip builds it for the host tests.
set_source_files_properties(qd_wire.c qd_page.c PROPERTIES COMPILE_OPTIONS -O2)
