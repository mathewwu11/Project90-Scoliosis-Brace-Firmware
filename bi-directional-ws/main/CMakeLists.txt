idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_server mdns nvs_flash esp_wifi esp_netif esp_event esp_driver_gpio esp_hw_support)