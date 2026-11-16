# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "auth.c" "auth_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash efuse esp_timer)
