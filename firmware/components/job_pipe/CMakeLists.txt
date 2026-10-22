# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "job_pipe.c" "job_pipe_rip.c"
                       INCLUDE_DIRS "include"
                       REQUIRES ripjob urf_decode halftone esp_timer)
