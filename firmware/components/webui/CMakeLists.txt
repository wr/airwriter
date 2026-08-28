# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "webui.c"
                       INCLUDE_DIRS "include"
                       REQUIRES settings esp_http_server esp_wifi app_update esp_system)
