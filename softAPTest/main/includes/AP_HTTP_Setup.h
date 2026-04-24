#ifndef AP_HTTP_SETUP_H_
#define AP_HTTP_SETUP_H_

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#define WIFI_SSID "ESP32_Config"
#define WIFI_PASS "12345678"

void url_decode(char *dst, const char *src);
esp_err_t root_handler(httpd_req_t *req);
esp_err_t submit_handler(httpd_req_t *req);
void start_webserver(void);
void wifi_init_ap(void);


#endif /* AP_HTTP_SETUP_H_ */