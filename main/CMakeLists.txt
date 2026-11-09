# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS main.c config.c logbuf.c wifi.c bridge.c eth.c dns.c web.c web_util.c ota.c console.c improv.c mqtt.c system.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif esp_eth esp_driver_gpio esp_driver_uart esp_timer app_update nvs_flash
                  esp_http_server esp_http_client esp_https_ota esp_partition esp_app_format mbedtls lwip json
                  mqtt esp_pm spi_flash bootloader_support espcoredump
)

# The web UI is embedded gzip-compressed
set(WEB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(
    OUTPUT "${WEB_GZ}"
    COMMAND gzip -9 -n -c "${WEB_SRC}" > "${WEB_GZ}"
    DEPENDS "${WEB_SRC}"
    COMMENT "Compressing web UI"
    VERBATIM)
add_custom_target(web_ui DEPENDS "${WEB_GZ}")
add_dependencies(${COMPONENT_LIB} web_ui)
target_add_binary_data(${COMPONENT_LIB} "${WEB_GZ}" BINARY)
