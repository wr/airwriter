# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES cable_test testpage urf_decode resample halftone imagewriter ripjob job_pipe serial_out ipp_server settings webui net_provision printer_state led_grid dip_config pic_prog localtalk laser printers esp_wifi nvs_flash esp_netif app_update esp_timer esp_driver_gpio esp_driver_uart ota_health fw_update bootloader_support esp_partition memdiag)
