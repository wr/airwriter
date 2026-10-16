# SPDX-License-Identifier: GPL-3.0-or-later
# Built into the bootloader only (ESP-IDF looks for bootloader_components/).
idf_component_register(SRCS "boot_hooks.c"
                       PRIV_REQUIRES hal esp_rom soc)
