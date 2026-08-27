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
/*
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
*/
const char *html_page =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"  <meta charset=\"UTF-8\" />\n"
"  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\" />\n"
"  <title>WiFi Configuration – Project90</title>\n"
"  <style>\n"
"    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }\n"
"    body {\n"
"      min-height: 100vh;\n"
"      font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif;\n"
"      background: linear-gradient(135deg, #00487C 0%, #0066A1 60%, #B8D4E8 100%);\n"
"      display: flex;\n"
"      align-items: center;\n"
"      justify-content: center;\n"
"    }\n"
"    .card {\n"
"      width: 100%;\n"
"      max-width: 420px;\n"
"      margin: 0 16px;\n"
"      border-radius: 20px;\n"
"      overflow: hidden;\n"
"      box-shadow: 0 20px 60px rgba(0,0,0,0.25);\n"
"    }\n"
"    .card-header {\n"
"      background: #00487C;\n"
"      padding: 32px 36px 28px;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      align-items: center;\n"
"      gap: 10px;\n"
"    }\n"
"    .card-header h1 {\n"
"      color: #fff;\n"
"      font-size: 1.2rem;\n"
"      font-weight: 600;\n"
"      letter-spacing: 0.02em;\n"
"    }\n"
"    .card-header p {\n"
"      color: #B8D4E8;\n"
"      font-size: 0.85rem;\n"
"    }\n"
"    .card-body {\n"
"      background: #D5E5F1;\n"
"      padding: 32px 36px;\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 22px;\n"
"    }\n"
"    .field {\n"
"      display: flex;\n"
"      flex-direction: column;\n"
"      gap: 7px;\n"
"    }\n"
"    .field label {\n"
"      font-size: 0.82rem;\n"
"      font-weight: 600;\n"
"      color: #00487C;\n"
"    }\n"
"    .field input {\n"
"      width: 100%;\n"
"      padding: 12px 16px;\n"
"      border-radius: 10px;\n"
"      border: 1.5px solid #B8D4E8;\n"
"      background: #fff;\n"
"      color: #00487C;\n"
"      font-size: 0.9rem;\n"
"      outline: none;\n"
"      transition: border-color 0.15s;\n"
"      font-family: inherit;\n"
"    }\n"
"    .field input:focus {\n"
"      border-color: #0066A1;\n"
"    }\n"
"    .connect-btn {\n"
"      width: 100%;\n"
"      padding: 13px;\n"
"      border-radius: 10px;\n"
"      border: none;\n"
"      background: #00487C;\n"
"      color: #fff;\n"
"      font-size: 0.9rem;\n"
"      font-weight: 600;\n"
"      letter-spacing: 0.03em;\n"
"      cursor: pointer;\n"
"      transition: background 0.2s;\n"
"      font-family: inherit;\n"
"    }\n"
"    .connect-btn:hover {\n"
"      background: #0066A1;\n"
"    }\n"
"    .footer {\n"
"      position: fixed;\n"
"      bottom: 20px;\n"
"      left: 0;\n"
"      width: 100%;\n"
"      text-align: center;\n"
"      font-size: 0.72rem;\n"
"      color: #D5E5F1;\n"
"      letter-spacing: 0.01em;\n"
"    }\n"
"  </style>\n"
"</head>\n"
"<body>\n"
"  <div class=\"card\">\n"
"    <div class=\"card-header\">\n"
"      <h1>WiFi Configuration</h1>\n"
"      <p>Connect your device to a wireless network</p>\n"
"    </div>\n"
"    <form class=\"card-body\" method=\"POST\" action=\"/submit\">\n"
"      <div class=\"field\">\n"
"        <label for=\"ssid\">WiFi Name</label>\n"
"        <input id=\"ssid\" type=\"text\" name=\"ssid\" placeholder=\"Enter network name\" autocomplete=\"off\" required />\n"
"      </div>\n"
"      <div class=\"field\">\n"
"        <label for=\"password\">WiFi Password</label>\n"
"        <input id=\"password\" type=\"password\" name=\"password\" placeholder=\"Enter password\" autocomplete=\"current-password\" />\n"
"      </div>\n"
"      <button class=\"connect-btn\" type=\"submit\">Connect</button>\n"
"    </form>\n"
"  </div>\n"
"  <p class=\"footer\">Project90 Scoliosis Brace Tension Sensor &mdash; Network Setup</p>\n"
"</body>\n"
"</html>\n";

// Entered WiFi credentials will be stored here
char decoded_ssid[64] = {0};
char decoded_pass[64] = {0};

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
        
        // Make sure that string is empty
        memset(decoded_ssid, 0, sizeof(decoded_ssid));
        memset(decoded_pass, 0, sizeof(decoded_pass));

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

    return ESP_OK;
}

/* ===================== START SERVER ===================== */
httpd_handle_t start_apserver(void)
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

    esp_netif_ip_info_t ip_info;
    
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if(ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to get AP netif handle");
        return server;
    }

    esp_netif_get_ip_info(ap_netif, &ip_info);

    ESP_LOGI(TAG, "AP IP Address: " IPSTR, IP2STR(&ip_info.ip));

    return server;
}

/* ===================== WIFI INIT (AP MODE) ===================== */
void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .channel = 1,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    if (strlen(AP_PASS) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Access Point started");
    ESP_LOGI(TAG, "SSID: %s", AP_SSID);
    ESP_LOGI(TAG, "IP: 192.168.4.1");

}

char* copy_ssid(void) {
    if(decoded_ssid[0] == '\0') {
        return "";
    }
    return decoded_ssid;
}

char* copy_pass(void) {
    if(decoded_pass[0] == '\0') {
        return "";
    }
    return decoded_pass;
}

// 

void stop_ap(void) {
    esp_wifi_set_mode(WIFI_MODE_NULL); // disable WiFi before stopping to ensure proper cleanup
    esp_wifi_stop();
    esp_wifi_restore();

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif) {
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_destroy(ap_netif);
    }

    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait for WiFi to fully stop
}

char* get_ap_ip(void) {
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");

    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to get AP netif handle");
        return "";
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);

    static char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    return ip_str;
}

// EOF