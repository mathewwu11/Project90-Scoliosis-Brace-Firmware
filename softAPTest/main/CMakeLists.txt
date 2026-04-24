idf_component_register(SRCS "softap_example_main.c" "AP_HTTP_Setup.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_http_server
                    INCLUDE_DIRS "." "includes")
