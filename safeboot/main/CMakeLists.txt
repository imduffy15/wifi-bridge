# SPDX-License-Identifier: Apache-2.0
# Shares the settings, Ethernet, captive DNS, HTTP helpers and update code with the firmware in ../../main
set(FW "${CMAKE_CURRENT_SOURCE_DIR}/../../main")
idf_component_register(
    SRCS safeboot.c ${FW}/config.c ${FW}/eth.c ${FW}/dns.c ${FW}/web_util.c ${FW}/ota.c
    INCLUDE_DIRS "." "${FW}"
    PRIV_REQUIRES esp_wifi esp_netif esp_eth esp_driver_gpio esp_timer app_update nvs_flash esp_http_server
                  esp_http_client esp_partition esp_app_format mbedtls lwip json spi_flash bootloader_support
)

set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/index.html")
set(WEB_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${WEB_GZ}"
    COMMAND gzip -9 -n -c "${WEB_SRC}" > "${WEB_GZ}"
    DEPENDS "${WEB_SRC}"
    COMMENT "Compressing safeboot page"
    VERBATIM)
add_custom_target(safeboot_page DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} safeboot_page)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
