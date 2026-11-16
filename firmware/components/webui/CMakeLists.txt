# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "webui.c" "webui_auth.c"
                       INCLUDE_DIRS "include"
                       REQUIRES auth settings fw_update esp_http_server esp_wifi esp_system led_grid memdiag logbook)
