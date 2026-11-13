# SPDX-License-Identifier: GPL-3.0-or-later
# The printers' icons, tools/icon/mkicons.py's PNGs; ipp_server.c's ICON_SETS names the same list.
set(icons icon stylewriter iwlq sworiginal sw2 sw1200 csw1500 csw2200
          lw lw2 plw lw4600 lwselect lwpro400 lwpro600 lwpro810 lw12640 lw8500 clw plw300)
set(icon_files "")
foreach(name ${icons})
    foreach(size 48 128 512)
        list(APPEND icon_files "icons/${name}-${size}.png")
    endforeach()
endforeach()

idf_component_register(SRCS "ipp_req.c" "ipp_job.c" "ipp_http.c" "ipp_server.c"
                       INCLUDE_DIRS "include"
                       EMBED_FILES ${icon_files}
                       REQUIRES ipp_attrs printer_caps esp_timer esp_hw_support lwip mdns)
