idf_component_register(SRCS "token_monitor.c"
                        INCLUDE_DIRS "."
                        REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_timer)
