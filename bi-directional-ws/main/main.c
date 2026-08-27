/*
 * ESP32 WebSocket sensor server (ESP-IDF)
 *
 * - Connects to WiFi in station mode
 * - Advertises itself via mDNS as <MDNS_HOSTNAME>.local
 * - Hosts a WebSocket endpoint at ws://<host>/ws
 * - Periodically broadcasts sensor readings to the connected client
 * - Accepts JSON commands from the client to change settings (interval, LED)
 *
 * EDIT BEFORE FLASHING:
 *   - WIFI_SSID / WIFI_PASS below
 *   - MDNS_HOSTNAME if you want something other than "esp32-sensor"
 *   - LED_GPIO if your board's LED is on a different pin
 *   - read_sensor() to read your actual sensor instead of the stub value
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "mdns.h"
#include "cJSON.h"
#include "esp_http_server.h"

/* ---------- User configuration ---------- */
#define WIFI_SSID           "49_24"
#define WIFI_PASS           "12345678"
#define WIFI_MAX_RETRY      10

#define MDNS_HOSTNAME        "esp32-sensor"
#define MDNS_INSTANCE_NAME   "ESP32 Sensor Server"

#define LED_GPIO             GPIO_NUM_2

/* ---------- Globals ---------- */
static const char *TAG = "ws_sensor";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
static int s_retry_num = 0;

static httpd_handle_t server = NULL;
static int client_fd = -1;                 /* single-client assumption */

static volatile int sample_interval_ms = 1000;
static volatile bool led_enabled = true;

/* ---------- Sensor stub ---------- */
/* Replace this with a real read (ADC, I2C, etc). Returns dummy data for now. */
static float read_sensor(void)
{
    static float fake_temp = 22.0f;
    fake_temp += ((esp_random() % 100) / 100.0f - 0.5f) * 0.3f;
    return fake_temp;
}

/* ---------- WiFi ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        client_fd = -1; /* connection is gone, drop any stale client fd */
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    }
}

static void start_mdns_service(void)
{
    esp_err_t err = mdns_init();
    if (err) {
        ESP_LOGE(TAG, "mDNS init failed: %d", err);
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set(MDNS_INSTANCE_NAME);
    mdns_service_add(NULL, "_ws", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", MDNS_HOSTNAME);
}

/* ---------- WebSocket handler ---------- */
static void apply_command(cJSON *root, int fd)
{
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    cJSON *value = cJSON_GetObjectItem(root, "value");

    if (!cmd || !cJSON_IsString(cmd)) {
        ESP_LOGW(TAG, "Command missing 'cmd' field");
        return;
    }

    cJSON *ack = cJSON_CreateObject();
    bool ok = true;

    if (strcmp(cmd->valuestring, "set_interval") == 0 && value && cJSON_IsNumber(value)) {
        int v = value->valueint;
        if (v < 50) v = 50; /* sane floor so it can't be set to 0/negative */
        sample_interval_ms = v;
        ESP_LOGI(TAG, "Interval set to %d ms", sample_interval_ms);
    } else if (strcmp(cmd->valuestring, "set_led") == 0 && value) {
        led_enabled = cJSON_IsTrue(value);
        gpio_set_level(LED_GPIO, led_enabled ? 1 : 0);
        ESP_LOGI(TAG, "LED set to %d", led_enabled);
    } else if (strcmp(cmd->valuestring, "get_status") == 0) {
        /* no state change, just fall through to the ack below */
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd->valuestring);
        ok = false;
    }

    cJSON_AddStringToObject(ack, "status", ok ? "ok" : "error");
    cJSON_AddStringToObject(ack, "cmd", cmd->valuestring);
    cJSON_AddNumberToObject(ack, "interval_ms", sample_interval_ms);
    cJSON_AddBoolToObject(ack, "led", led_enabled);

    char *out = cJSON_PrintUnformatted(ack);
    if (out) {
        httpd_ws_frame_t resp = {
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)out,
            .len = strlen(out),
        };
        httpd_ws_send_frame_async(server, fd, &resp);
        free(out);
    }
    cJSON_Delete(ack);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* handshake completed — remember this client's fd */
        client_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "Client connected, fd=%d", client_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    /* first call with a NULL buffer just fetches the frame length */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recv frame len failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (ws_pkt.len == 0) {
        return ESP_OK; /* control frame (ping/pong/close), nothing to parse */
    }

    uint8_t *buf = calloc(1, ws_pkt.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recv frame failed: %s", esp_err_to_name(ret));
        free(buf);
        return ret;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
        ESP_LOGI(TAG, "Received: %s", (char *)buf);
        cJSON *root = cJSON_Parse((char *)buf);
        if (root) {
            apply_command(root, httpd_req_to_sockfd(req));
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "Failed to parse JSON command");
        }
    } else if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Client sent close frame");
        client_fd = -1;
    }

    free(buf);
    return ESP_OK;
}

static const httpd_uri_t ws_uri = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
};

static void start_ws_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &ws_uri);
        ESP_LOGI(TAG, "WebSocket server started on port %d", config.server_port);
    } else {
        ESP_LOGE(TAG, "Failed to start httpd server");
    }
}

/* ---------- Sensor broadcast task ---------- */
static void broadcast_reading(float temp)
{
    if (client_fd < 0 || server == NULL) {
        return; /* no client connected */
    }

    char payload[64];
    int len = snprintf(payload, sizeof(payload), "{\"temp\":%.2f}", temp);

    httpd_ws_frame_t ws_pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = (size_t)len,
    };

    esp_err_t ret = httpd_ws_send_frame_async(server, client_fd, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Send failed (client likely disconnected): %s", esp_err_to_name(ret));
        client_fd = -1;
    }
}

static void sensor_task(void *arg)
{
    while (1) {
        float reading = read_sensor();
        broadcast_reading(reading);
        vTaskDelay(pdMS_TO_TICKS(sample_interval_ms));
    }
}

/* ---------- WiFi init ---------- */
static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished, connecting to SSID:%s", WIFI_SSID);
}

static void got_ip_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

    start_mdns_service();
    start_ws_server();

    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, led_enabled ? 1 : 0);

    wifi_init_sta();

    /* got_ip_handler does the actual mDNS/server/task startup once we have an IP */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_handler, NULL, NULL));

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to WiFi after %d attempts", WIFI_MAX_RETRY);
    }
}