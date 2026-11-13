# SPDX-License-Identifier: GPL-3.0-or-later
# W-1102: QuickDraw LaserWriters on the printer port.
idf_component_register(SRCS "qd_model.c" "qd_wire.c" "qd_status.c" "qd_page.c" "qd_session.c"
                            "qd_link.c" "qdlaser.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps halftone ripjob stylewriter bigmem serial_out job_pipe
                                esp_timer esp_rom esp_driver_gpio esp_driver_spi esp_driver_uart
                                esp_hw_support hal soc)
