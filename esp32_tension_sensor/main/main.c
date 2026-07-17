#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_random.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "AP_HTTP_Setup.h"
#include "ssd1306.h"

#define SELPIN     GPIO_NUM_10
#define DATAOUT    GPIO_NUM_11
#define DATAIN     GPIO_NUM_12
#define SPICLOCK   GPIO_NUM_13

#define I2C_PORT                I2C_NUM_0
#define I2C_SDA_GPIO            21
#define I2C_SCL_GPIO            26
#define I2C_FREQ_HZ             100000

#define DS3231_I2C_ADDR         0x68

#define DS3231_REG_SECONDS      0x00
#define DS3231_REG_MINUTES      0x01
#define DS3231_REG_HOURS        0x02
#define DS3231_REG_DAY          0x03
#define DS3231_REG_DATE         0x04
#define DS3231_REG_MONTH        0x05
#define DS3231_REG_YEAR         0x06

static const char *TAG = "ws_server";

static int client_fd = -1;
static httpd_handle_t server = NULL;
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static i2c_master_dev_handle_t ds3231_handle;
static i2c_master_bus_handle_t bus_handle;

typedef struct
{
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t date;
    uint8_t month;
    uint16_t year;
} ds3231_time_t;

char user_ssid[64] = {0};
char user_password[64] = {0};

// Function Prototypes
void wifi_init(char *wifi_ssid, char *wifi_pass);

/* ---------------- RTC Handler ---------------- */
static esp_err_t rtc_i2c_init(void)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_I2C_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(
        bus_handle,
        &dev_config,
        &ds3231_handle
    ));

    ESP_LOGI(TAG, "I2C initialized");

    return ESP_OK;
}

static uint8_t dec_to_bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

static uint8_t bcd_to_dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

static esp_err_t ds3231_get_time(ds3231_time_t *time)
{
    uint8_t reg = DS3231_REG_SECONDS;
    uint8_t data[7];

    ESP_ERROR_CHECK(i2c_master_transmit_receive(
        ds3231_handle,
        &reg,
        1,
        data,
        sizeof(data),
        -1
    ));

    time->seconds = bcd_to_dec(data[0] & 0x7F);
    time->minutes = bcd_to_dec(data[1]);
    time->hours   = bcd_to_dec(data[2] & 0x3F);
    time->day     = bcd_to_dec(data[3]);
    time->date    = bcd_to_dec(data[4]);
    time->month   = bcd_to_dec(data[5] & 0x1F);
    time->year    = 2000 + bcd_to_dec(data[6]);

    return ESP_OK;
}

/* ---------------- ADC Handler ---------------- */
int read_adc(int channel)
{
    int adcvalue = 0;
    uint8_t commandbits = 0b11000000;

    // Allow channel selection
    commandbits |= ((channel) << 3);

    // Select ADC
    gpio_set_level(SELPIN, 0);

    // Send command bits (bits 7 down to 3)
    for (int i = 7; i >= 3; i--)
    {
        gpio_set_level(DATAOUT,
                      (commandbits & (1 << i)) ? 1 : 0);

        gpio_set_level(SPICLOCK, 1);
        gpio_set_level(SPICLOCK, 0);
    }

    // Ignore two null bits
    gpio_set_level(SPICLOCK, 1);
    gpio_set_level(SPICLOCK, 0);

    gpio_set_level(SPICLOCK, 1);
    gpio_set_level(SPICLOCK, 0);

    // Read 12 bits
    for (int i = 11; i >= 0; i--)
    {
        adcvalue |= (gpio_get_level(DATAIN) << i);

        gpio_set_level(SPICLOCK, 1);
        gpio_set_level(SPICLOCK, 0);
    }

    // Disable ADC
    gpio_set_level(SELPIN, 1);

    return adcvalue;
}

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

char* get_ws_ip(void) {
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");

    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to get WS netif handle");
        return "";
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif, &ip_info);

    static char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    return ip_str;
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

            int reading1 = read_adc(0);
            int reading2 = read_adc(1);
            int reading3 = read_adc(2);
            int reading4 = read_adc(3);
            int reading5 = read_adc(4);
            int reading6 = read_adc(5);
            
            ds3231_time_t current_time;

            ds3231_get_time(&current_time);

            sprintf(msg, 
                "{\"Time\":%04d-%02d-%02dT%02d:%02d:%02d-06:00, \"Reading 1\":%d, \"Reading 2\":%d, \"Reading 3\":%d, \"Reading 4\":%d, \"Reading 5\":%d, \"Reading 6\":%d}", 
                current_time.year, 
                current_time.month, 
                current_time.date, 
                current_time.hours, 
                current_time.minutes, 
                current_time.seconds, 
                reading1, 
                reading2, 
                reading3, 
                reading4, 
                reading5, 
                reading6);

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
    // This function initializes the SSD1306 OLED display. 
    // Additionally, it initializes the I2C channel, which is used for the RTC module.
    bus_handle = init_ssd1306();

    rtc_i2c_init();


    // Configure output pins
    gpio_config_t output_conf = {
        .pin_bit_mask =
            (1ULL << SELPIN) |
            (1ULL << DATAOUT) |
            (1ULL << SPICLOCK),
        .mode = GPIO_MODE_OUTPUT,
    };

    gpio_config(&output_conf);

    // Configure input pin
    gpio_config_t input_conf = {
        .pin_bit_mask = (1ULL << DATAIN),
        .mode = GPIO_MODE_INPUT,
    };

    gpio_config(&input_conf);

    // Initial states
    gpio_set_level(SELPIN, 1);
    gpio_set_level(DATAOUT, 0);
    gpio_set_level(SPICLOCK, 0);
    
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

    // Display AP IP address on OLED
    clear_display();
    ssd1306_print_str(0, 0, "ID: SBTS_Config", false);
    ssd1306_print_str(0, 16, "P: 12345678", false);
    ssd1306_print_str(0, 32, "IP Address:", false);
    ssd1306_print_str(0, 48, get_ap_ip(), false);
    ssd1306_display();
    
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

    clear_server();
    stop_ap();
    clear_display();
    ssd1306_display();

    wifi_init(user_ssid, user_password);

    server = start_webserver();

    ssd1306_print_str(0, 0, "Server ID", false);
    ssd1306_print_str(0, 32, get_ws_ip(), false);
    ssd1306_display();

    xTaskCreate(telemetry_task,
                "telemetry",
                4096,
                NULL,
                5,
                NULL);
}