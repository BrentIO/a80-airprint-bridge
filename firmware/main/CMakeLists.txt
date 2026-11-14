idf_component_register(SRCS "main.c" "ipp.c" "httpd.c" "weblog.c" "urfconv.c" "printer.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_netif nvs_flash esp_event lwip esp_psram esp_timer bt esp_driver_gpio)
