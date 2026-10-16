# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ota_health.c" "ota_health_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update esp_timer)
