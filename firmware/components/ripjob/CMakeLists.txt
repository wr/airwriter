# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ripjob.c"
                       INCLUDE_DIRS "include"
                       REQUIRES urf_decode resample halftone imagewriter)
