#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_random.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "AP_HTTP_Setup.h"

// changing ssid and password requires full clean and rebuild of the project
// compiler lowkirk trips out
#define WIFI_SSID "49_24"
#define WIFI_PASS "12345678"

static const char *TAG = "ws_server";

static int client_fd = -1;
static httpd_handle_t server = NULL;
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

char user_ssid[64] = {0};
char user_password[64] = {0};

// Function Prototypes
void wifi_init(char *wifi_ssid, char *wifi_pass);

/* ---------------- WiFi Event Handler ---------------- */
// Despite the fact that the event handler is registered for both WIFI_EVENT and IP_EVENT.
static void wifi_event_handler(void* arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ESP_LOGI(TAG, "WiFi connected");
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) 
    {
        ESP_LOGI(TAG, "Invalid SSID or Password. Restarting device...");
        esp_restart();
    }

}

/* ---------------- WiFi Init ---------------- */

void wifi_init(char *wifi_ssid, char *wifi_pass)
{
    wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta_netif, "P90-SBTS-Device");

    //wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    //esp_wifi_init(&cfg);

    //esp_event_handler_register(IP_EVENT,
                               //IP_EVENT_STA_GOT_IP,
                               //wifi_event_handler,
                               //NULL);

    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL);

    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL);
    
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = {0},
            .password = {0},
            .failure_retry_cnt = 3
        }
    };

    strncpy((char*)wifi_config.sta.ssid, (const char*)wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, (const char*)wifi_pass, sizeof(wifi_config.sta.password) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        false,
        true,
        portMAX_DELAY
    );
}

/* ---------------- WebSocket Handler ---------------- */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        ESP_LOGI(TAG, "WebSocket handshake complete");

        client_fd = httpd_req_to_sockfd(req);

        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    frame.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_recv_frame(req, &frame, 0);

    if (frame.len)
    {
        frame.payload = malloc(frame.len + 1);

        httpd_ws_recv_frame(req, &frame, frame.len);

        frame.payload[frame.len] = 0;

        ESP_LOGI(TAG, "Received: %s", (char*)frame.payload);

        free(frame.payload);
    }

    return ESP_OK;
}

/* ---------------- Telemetry Task ---------------- */

void telemetry_task(void *arg)
{
    while (1)
    {
        if (server && client_fd != -1)
        {
            char msg[256];

            int value = esp_random() % 100;

            sprintf(msg, "{\"Time\":%d, \"Reading 1\":%d, \"Reading 2\":%d, \"Reading 3\":%d, \"Reading 4\":%d, \"Reading 5\":%d, \"Reading 6\":%d}", value, value, value, value, value, value, value);

            httpd_ws_frame_t frame = {
                .payload = (uint8_t*)msg,
                .len = strlen(msg),
                .type = HTTPD_WS_TYPE_TEXT
            };

            httpd_ws_send_frame_async(server, client_fd, &frame);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------- Start Server ---------------- */

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true
        };

        httpd_register_uri_handler(server, &ws_uri);
    }

    return server;
}

void clear_server(void)
{
    if (server) 
    {
        httpd_stop(server);
        server = NULL;
    }
}

/* ---------------- Main ---------------- */

void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Initialize TCP/IP stack
    esp_netif_init();
    esp_event_loop_create_default();

    // Start WiFi AP
    wifi_init_ap();

    // Start ap server
    server = start_apserver();
    
    while(1){
        // make sure buffers are cleared before copying new values
        memset(user_ssid, 0, sizeof(user_ssid));
        memset(user_password, 0, sizeof(user_password));
        
        strcpy(user_ssid, copy_ssid());
        strcpy(user_password, copy_pass());

        if(user_ssid[0] != '\0' && user_password[0] != '\0'){
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    //while (!ap_stopped) {
    //    vTaskDelay(pdMS_TO_TICKS(10));
    //    ESP_LOGI(TAG, "Waiting for AP to stop...");
    //}

    clear_server();
    stop_ap();

    wifi_init(user_ssid, user_password);
    //wifi_init(WIFI_SSID, WIFI_PASS);

    server = start_webserver();

    xTaskCreate(telemetry_task,
                "telemetry",
                4096,
                NULL,
                5,
                NULL);
}