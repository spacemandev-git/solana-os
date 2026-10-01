include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

idf_component_register(
    SRCS ${SOLANA_OS_SRCS} ${SOLANA_OS_DEVICE_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES bsp nvs_flash esp_wifi esp_netif esp_event console esp_timer mbedtls
                  esp_app_format spi_flash fatfs vfs esp_http_client esp-tls json)

# Mesh messages are untrusted JSON and cJSON parses recursively (~80 B of
# stack per level): cap the depth so a crafted frame can't overflow a task.
# os_mesh also drops anything nested deeper than 8 before parsing.
idf_component_get_property(json_lib json COMPONENT_LIB)
target_compile_definitions(${json_lib} PRIVATE CJSON_NESTING_LIMIT=16)
