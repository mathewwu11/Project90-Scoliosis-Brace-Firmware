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
#include "AP_HTTP_Setup.h"

static const char *TAG = "ESP32_PORTAL";

/* ===================== HTML PAGE ===================== */
static const char *html_page =
"<!DOCTYPE html>"
"<html>"
"<head><meta name='viewport' content='width=device-width, initial-scale=1'></head>"
"<body>"
"<h2>Enter WiFi Credentials</h2>"
"<form method=\"POST\" action=\"/submit\">"
"SSID:<br><input type=\"text\" name=\"ssid\"><br><br>"
"Password:<br><input type=\"password\" name=\"password\"><br><br>"
"<input type=\"submit\" value=\"Send\">"
"</form>"
"</body>"
"</html>";

/* ===================== URL DECODE ===================== */
void url_decode(char *dst, const char *src)
{
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            (isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2]))) {

            a = src[1];
            b = src[2];

            a = (a >= 'A') ? (a & ~0x20) - 'A' + 10 : a - '0';
            b = (b >= 'A') ? (b & ~0x20) - 'A' + 10 : b - '0';

            *dst++ = 16*a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* ===================== ROOT HANDLER ===================== */
esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ===================== SUBMIT HANDLER ===================== */
esp_err_t submit_handler(httpd_req_t *req)
{
    char buf[200];

    int ret = httpd_req_recv(req, buf, req->content_len);
    if (ret <= 0) {
        ESP_LOGE(TAG, "Failed to receive POST data");
        return ESP_FAIL;
    }

    buf[ret] = '\0';

    ESP_LOGI(TAG, "Raw POST data: %s", buf);

    char ssid[64] = {0};
    char pass[64] = {0};

    char *ssid_ptr = strstr(buf, "ssid=");
    char *pass_ptr = strstr(buf, "password=");

    if (ssid_ptr && pass_ptr) {
        ssid_ptr += 5;
        pass_ptr += 9;

        char *ssid_end = strchr(ssid_ptr, '&');

        if (ssid_end) {
            strncpy(ssid, ssid_ptr, ssid_end - ssid_ptr);
            ssid[ssid_end - ssid_ptr] = '\0';
        } else {
            strcpy(ssid, ssid_ptr);
        }

        strcpy(pass, pass_ptr);

        char decoded_ssid[64];
        char decoded_pass[64];

        url_decode(decoded_ssid, ssid);
        url_decode(decoded_pass, pass);

        ESP_LOGI(TAG, "Parsed SSID: %s", decoded_ssid);
        ESP_LOGI(TAG, "Parsed PASS: %s", decoded_pass);
    }

    // Send response BEFORE shutting down
    httpd_resp_send(req, "Received! Device will stop.", HTTPD_RESP_USE_STRLEN);

    // Give browser time to receive response
    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Stopping AP Server");

    esp_wifi_stop();

    return ESP_OK;
}

/* ===================== START SERVER ===================== */
void start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK) {

        httpd_uri_t root = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = root_handler,
            .user_ctx = NULL
        };

        httpd_uri_t submit = {
            .uri      = "/submit",
            .method   = HTTP_POST,
            .handler  = submit_handler,
            .user_ctx = NULL
        };

        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &submit);
    }
}

/* ===================== WIFI INIT (AP MODE) ===================== */
void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = 1,
            .password = WIFI_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    if (strlen(WIFI_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Access Point started");
    ESP_LOGI(TAG, "SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "IP: 192.168.4.1");
}


// EOF