# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "webui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES settings fw_update esp_http_server esp_wifi esp_system)
