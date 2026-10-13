# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "pic_prog.c" "pic_prog_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio
                       EMBED_TXTFILES "tashtalk-2.1.3.hex")
