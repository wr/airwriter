# SPDX-License-Identifier: GPL-3.0-or-later
# logbook_core.c is pure (tools/rip tests it on the host); logbook.c is the board's.
# The wrap is how a crash reaches the log: the panic handler writes to the USB peripheral
# directly, never through esp_log.
idf_component_register(SRCS "logbook_core.c" "logbook.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_partition heap esp_system freertos log spi_flash esp_timer esp_app_format xtensa esp_rom)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
