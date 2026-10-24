# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "localtalk.c" "lt_node.c" "lt_proto.c" "lt_tashtalk.c" "pap_client.c" "pap_relay.c"
                            "et_proto.c" "et_link.c" "et_transport.c" "at_printer.c" "ethertalk.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_uart esp_driver_gpio esp_timer nvs_flash esp_hw_support heap
                                lwip esp_event mbedtls esp_websocket_client esp_wifi)

# W-1078: AppleTalk frames off the board's own Wi-Fi reach EtherTalk through
# lwIP's unknown-protocol hook (include/et_radio_hook.h).
idf_component_get_property(lwip lwip COMPONENT_LIB)
target_compile_options(${lwip} PRIVATE "-I${CMAKE_CURRENT_LIST_DIR}/include")
target_compile_definitions(${lwip} PRIVATE "-DESP_IDF_LWIP_HOOK_FILENAME=\"et_radio_hook.h\"")
