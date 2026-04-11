idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_https_server nvs_flash esp_timer esp_netif esp_eth esp_wifi)
