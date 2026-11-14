# SPDX-License-Identifier: GPL-3.0-or-later
# sw_model.c, sw_code.c, sw_page.c, sw_session.c, sw_status.c and sw_testpage.c are pure C
# (tools/rip builds them on the host); stylewriter.c is the board's: the session's line over
# serial_out, a job's place on the pipeline and the test page's session. The test page itself is
# testpage's; sw_testpage.c is only a StyleWriter's geometry for it.
idf_component_register(SRCS "sw_model.c" "sw_code.c" "sw_page.c" "sw_session.c" "sw_status.c"
                            "sw_testpage.c" "stylewriter.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps halftone ripjob bigmem serial_out job_pipe esp_timer
                                imagewriter testpage)
# W-1143: the rip's per-row code is built for speed rather than ESP-IDF's debug optimisation,
# as tools/rip builds it for the host tests.
set_source_files_properties(sw_code.c sw_page.c PROPERTIES COMPILE_OPTIONS -O2)
