# SPDX-License-Identifier: GPL-3.0-or-later
# The test page every printer prints (testpage.h, W-1140), and the ImageWriter's geometry for it
# (tp_iw.c). Pure C; tools/rip builds it on the host. tp_face.h is its face; tp_words.h and
# tp_icons.h (testpage/tools/mkart.py) its letterhead; tp_type.h (testpage/tools/mktype.py) its
# type tester; tp_pictures.h (tools/rip/mkscene.c, from tp_scene.c, which the board does not
# build) the still life its screens show. tp_unpack.c inflates them with the ROM's tinfl here and
# zlib on the host.
idf_component_register(SRCS "testpage.c" "tp_iw.c" "tp_unpack.c"
                       INCLUDE_DIRS "include"
                       REQUIRES halftone imagewriter ripjob resample bigmem esp_rom)
