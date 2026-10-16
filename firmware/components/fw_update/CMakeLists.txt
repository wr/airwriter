# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "fw_update.c" "fw_update_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update esp_https_ota esp_http_client esp-tls mbedtls
                                bootloader_support esp_timer settings led_grid ota_health)
