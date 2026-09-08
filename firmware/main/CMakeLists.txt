idf_component_register(SRCS "token_monitor.c" "gc9a01.c" "config.c"
                        INCLUDE_DIRS "."
                        REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer esp_http_client json
                                 esp_driver_spi esp_driver_gpio)
