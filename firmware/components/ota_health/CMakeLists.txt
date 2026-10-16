# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "ota_health.c" "ota_health_core.c" "known_good_core.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_update esp_partition esp_timer)

# W-1059: `idf.py flash` writes a blank known-good record beside IDF's blank
# otadata (app_update does that one the same way), so a USB-flashed board
# boots, and resets to, the app it was just given in ota_0.
partition_table_get_partition_info(kg_offset "--partition-name known_good" "offset")
partition_table_get_partition_info(kg_size "--partition-name known_good" "size")
if(kg_offset AND kg_size)
    idf_build_get_property(build_dir BUILD_DIR)
    idf_build_get_property(python PYTHON)
    idf_component_get_property(partition_table_dir partition_table COMPONENT_DIR)
    set(kg_blank ${build_dir}/known_good_initial.bin)
    add_custom_command(OUTPUT ${kg_blank}
        COMMAND ${python} ${partition_table_dir}/gen_empty_partition.py ${kg_size} ${kg_blank})
    add_custom_target(blank_known_good ALL DEPENDS ${kg_blank})
    add_dependencies(flash blank_known_good)
    esptool_py_flash_target_image(flash known_good "${kg_offset}" "${kg_blank}")
endif()
