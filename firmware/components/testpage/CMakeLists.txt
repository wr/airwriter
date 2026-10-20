# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "testpage.c" "tp_scene.c"
                       INCLUDE_DIRS "include"
                       REQUIRES halftone imagewriter ripjob)
