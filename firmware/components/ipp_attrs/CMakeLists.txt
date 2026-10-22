# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ipp_enc.c" "ipp_attrs.c"
                       INCLUDE_DIRS "include"
                       REQUIRES printer_caps)
