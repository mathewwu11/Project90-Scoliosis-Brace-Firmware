#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "mdns.h"
#include "cJSON.h"

#include "esp_http_server.h"
#include "AP_HTTP_Setup.h"
#include "ssd1306.h"

#define SELPIN     GPIO_NUM_10
#define DATAOUT    GPIO_NUM_11
#define DATAIN     GPIO_NUM_12
#define SPICLOCK   GPIO_NUM_13

#define PIN_NUM_MISO 12   // MCP3208 DOUT
#define PIN_NUM_MOSI 11   // MCP3208 DIN
#define PIN_NUM_CLK  13   // MCP3208 CLK
#define PIN_NUM_CS   10    // MCP3208 CS/SHDN

#define SPI_HOST_USED SPI2_HOST   // VSPI on most ESP32 dev boards
#define ADC_CHANNEL   0           // channel we want to read

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

#define MDNS_HOSTNAME        "SBTS"
#define MDNS_INSTANCE_NAME   "SBTS Server"



static const char *TAG = "ws_server";

static int s_retry_num = 0;
static int client_fd = -1;
static httpd_handle_t server = NULL;
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static i2c_master_dev_handle_t ds3231_handle;
static i2c_master_bus_handle_t bus_handle;

static spi_device_handle_t spi_handle;

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
static void got_ip_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

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

static void mcp3208_spi_init(void)
{
    esp_err_t ret;

    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1 * 1000 * 1000,  // 1 MHz, well within MCP3208 limits
        .mode = 0,                          // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
    };

    ret = spi_bus_initialize(SPI_HOST_USED, &buscfg, SPI_DMA_DISABLED);
    ESP_ERROR_CHECK(ret);

    ret = spi_bus_add_device(SPI_HOST_USED, &devcfg, &spi_handle);
    ESP_ERROR_CHECK(ret);
}

/**
 * Read a single-ended channel (0-7) from the MCP3208.
 * Returns a 12-bit value (0-4095).
 */
static uint16_t mcp3208_read_channel(spi_device_handle_t spi, uint8_t channel)
{
    // MCP3208 command format:
    // Byte0: 0000 011 | D2   (start bit=1, single/diff=1, then MSB of channel)
    // Byte1: D1 D0 0000 00   (remaining channel bits, rest don't-care)
    // Byte2: 0000 0000       (clocks out remaining data bits)
    uint8_t tx_data[3];
    uint8_t rx_data[3] = {0};

    tx_data[0] = 0x06 | ((channel & 0x07) >> 2);
    tx_data[1] = (channel & 0x03) << 6;
    tx_data[2] = 0x00;

    spi_transaction_t t = {
        .length = 8 * 3,       // 3 bytes = 24 bits
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    esp_err_t ret = spi_device_transmit(spi, &t);
    ESP_ERROR_CHECK(ret);

    // 12-bit result spans the low nibble of rx_data[1] and all of rx_data[2]
    uint16_t value = ((rx_data[1] & 0x0F) << 8) | rx_data[2];
    return value;
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

    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &wifi_event_handler,
                                        NULL,
                                        NULL);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, 
                                                        IP_EVENT_STA_GOT_IP, 
                                                        &got_ip_handler, 
                                                        NULL, 
                                                        NULL));
    
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

// mDNS service initialization
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
    static char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%s.local", MDNS_HOSTNAME);
    return ip_str;
}

static void got_ip_handler(void *arg, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);

    start_mdns_service();

}


/* ---------------- Telemetry Task ---------------- */
// task that sends real-time telemetry data to the connected WebSocket client every second
void telemetry_task(void *arg)
{
    while (1)
    {
        if (server && client_fd != -1)
        {
            char msg[256];

            uint16_t reading1 = mcp3208_read_channel(spi_handle, ADC_CHANNEL);
            
            ds3231_time_t current_time;

            ds3231_get_time(&current_time);

            sprintf(msg, 
                "{\"Time\":%04d-%02d-%02dT%02d:%02d:%02d-06:00, \"Reading\":%4d}", 
                current_time.year, 
                current_time.month, 
                current_time.date, 
                current_time.hours, 
                current_time.minutes, 
                current_time.seconds, 
                reading1);

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

    ESP_LOGI(TAG, "Initializing SPI for MCP3208...");
    mcp3208_spi_init();

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