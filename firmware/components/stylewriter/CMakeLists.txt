# SPDX-License-Identifier: GPL-3.0-or-later
# sw_model.c, sw_code.c, sw_page.c, sw_session.c and sw_status.c are pure C (tools/rip builds them
# on the host); stylewriter.c is the board's: the session's line over serial_out and a job's
# place on the pipeline.
idf_component_register(SRCS "sw_model.c" "sw_code.c" "sw_page.c" "sw_session.c" "sw_status.c"
                            "stylewriter.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps halftone ripjob bigmem serial_out job_pipe esp_timer)
