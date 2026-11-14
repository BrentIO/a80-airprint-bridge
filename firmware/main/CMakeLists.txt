idf_component_register(SRCS "main.c" "ipp.c" "httpd.c" "weblog.c" "urfconv.c" "printer.c" "wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_event lwip esp_psram esp_timer bt esp_driver_gpio)

include(${CMAKE_SOURCE_DIR}/version.cmake)
target_compile_definitions(${COMPONENT_LIB} PRIVATE A80_VERSION="${A80_VERSION}" A80_COMMIT="${A80_COMMIT}")
