# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "auth.c" "auth_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES nvs_flash efuse esp_timer)
# The hashes run 10000 PBKDF2 rounds on a change: at the build's -Og they took
# several times as long.
set_source_files_properties(auth_core.c PROPERTIES COMPILE_OPTIONS "-O2")
