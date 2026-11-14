# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)

# W-1109: one source, a build per main board. Rev A's ESP32-S3-WROOM-1-N8R2 has quad PSRAM
# and rev 2026.11's N16R8 octal, which ESP-IDF fixes at build time, so neither image runs on
# the other board. `idf.py build` is rev A, as it always was. Rev 2026.11:
#   idf.py -B build-2026.11 -DAIRWRITER_BOARD=2026.11 build
# which layers sdkconfig.defaults.2026.11 on the usual defaults, keeps its own
# sdkconfig.2026.11, and names the image airwriter-2026.11: each board takes only images
# with its own name (fw_update).
set(AIRWRITER_BOARD "rev-a" CACHE STRING "The main board: rev-a or 2026.11")
set_property(CACHE AIRWRITER_BOARD PROPERTY STRINGS rev-a 2026.11)
if(AIRWRITER_BOARD STREQUAL "2026.11")
    if(NOT SDKCONFIG_DEFAULTS)
        set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
    endif()
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.2026.11")
    if(NOT SDKCONFIG)
        set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.2026.11")
    endif()
    set(AIRWRITER_PROJECT airwriter-2026.11)
elseif(AIRWRITER_BOARD STREQUAL "rev-a")
    set(AIRWRITER_PROJECT airwriter)
else()
    message(FATAL_ERROR "AIRWRITER_BOARD is rev-a or 2026.11, not ${AIRWRITER_BOARD}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${AIRWRITER_PROJECT})

# An sdkconfig keeps the board it was made for: building it as the other board would give
# one board's pins and PSRAM under the other's name.
if(AIRWRITER_BOARD STREQUAL "2026.11" AND NOT CONFIG_AIRWRITER_BOARD_2026_11)
    message(FATAL_ERROR "${SDKCONFIG} is a rev A sdkconfig; build rev 2026.11 with its own "
                        "(the default, sdkconfig.2026.11) in its own build directory")
elseif(AIRWRITER_BOARD STREQUAL "rev-a" AND NOT CONFIG_AIRWRITER_BOARD_REV_A)
    message(FATAL_ERROR "${SDKCONFIG} is a rev 2026.11 sdkconfig; pass -DAIRWRITER_BOARD=2026.11")
endif()
