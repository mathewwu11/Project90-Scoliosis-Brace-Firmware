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

#define WIFI_SSID "49_2.4"
#define WIFI_PASS "12345678"

static const char *TAG = "ws_server";

static httpd_handle_t server = NULL;
static int client_fd = -1;

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* ---------------- WiFi Event Handler ---------------- */

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
}

/* ---------------- WiFi Init ---------------- */

static void wifi_init()
{
    wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(IP_EVENT,
                               IP_EVENT_STA_GOT_IP,
                               wifi_event_handler,
                               NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS
        }
    };

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
            char msg[64];

            int value = esp_random() % 100;

            sprintf(msg, "{\"value\":%d}", value);

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

/* ---------------- Main ---------------- */

void app_main(void)
{
    nvs_flash_init();

    wifi_init();

    server = start_webserver();

    xTaskCreate(telemetry_task,
                "telemetry",
                4096,
                NULL,
                5,
                NULL);
}