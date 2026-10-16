# SPDX-License-Identifier: GPL-3.0-or-later
# Built into the bootloader only (ESP-IDF looks for bootloader_components/).
# known_good_core.c is the app's own (components/ota_health), so the record
# is read here exactly as the firmware writes it.
idf_component_register(SRCS "boot_hooks.c" "../../components/ota_health/known_good_core.c"
                       PRIV_INCLUDE_DIRS "../../components/ota_health/include"
                       PRIV_REQUIRES hal esp_rom soc log bootloader_support spi_flash esp_app_format)
