#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "DS3231";

// =========================
// I2C CONFIGURATION
// =========================

#define I2C_PORT                I2C_NUM_0
#define I2C_SDA_GPIO            21
#define I2C_SCL_GPIO            26
#define I2C_FREQ_HZ             100000

#define DS3231_I2C_ADDR         0x68

// =========================
// DS3231 REGISTERS
// =========================

#define DS3231_REG_SECONDS      0x00
#define DS3231_REG_MINUTES      0x01
#define DS3231_REG_HOURS        0x02
#define DS3231_REG_DAY          0x03
#define DS3231_REG_DATE         0x04
#define DS3231_REG_MONTH        0x05
#define DS3231_REG_YEAR         0x06

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

static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t ds3231_handle;

// =========================
// BCD CONVERSION
// =========================

static uint8_t dec_to_bcd(uint8_t val)
{
    return ((val / 10) << 4) | (val % 10);
}

static uint8_t bcd_to_dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

// =========================
// I2C INITIALIZATION
// =========================

static esp_err_t i2c_master_init(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &bus_handle));

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

// =========================
// WRITE TIME
// =========================

static esp_err_t ds3231_set_time(ds3231_time_t *time)
{
    uint8_t data[8];

    data[0] = DS3231_REG_SECONDS;

    data[1] = dec_to_bcd(time->seconds);
    data[2] = dec_to_bcd(time->minutes);
    data[3] = dec_to_bcd(time->hours);
    data[4] = dec_to_bcd(time->day);
    data[5] = dec_to_bcd(time->date);
    data[6] = dec_to_bcd(time->month);
    data[7] = dec_to_bcd(time->year - 2000);

    esp_err_t err = i2c_master_transmit(
        ds3231_handle,
        data,
        sizeof(data),
        pdMS_TO_TICKS(100)
    );

    printf("Transmit returned: %s\n", esp_err_to_name(err));

    return err;
}

// =========================
// READ TIME
// =========================

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

// =========================
// MAIN APPLICATION
// =========================

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    /*
    ds3231_time_t set_time = {
        .seconds = 0,
        .minutes = 10,
        .hours   = 17,   // 24-hour format
        .day     = 4,    // day of week, 1 = Sunday (or whatever convention you use)
        .date    = 26,
        .month   = 8,
        .year    = 2026
    };
    */

    if (i2c_master_probe(bus_handle, DS3231_I2C_ADDR, 1000) == ESP_OK)
    {
        printf("Found device at 0x%02X\n", DS3231_I2C_ADDR);
    }
    else
    {
        printf("Device not found at 0x%02X\n", DS3231_I2C_ADDR);
        return;
    }

    /*
    if (ds3231_set_time(&set_time) == ESP_OK)
    {
        ESP_LOGI(TAG, "Time set successfully");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to set time");
    }
    */

    while (1)
    {
        ds3231_time_t current_time;

        if (ds3231_get_time(&current_time) == ESP_OK)
        {
            ESP_LOGI(TAG,
                     "Time: %02d:%02d:%02d  Date: %02d/%02d/%04d",
                     current_time.hours,
                     current_time.minutes,
                     current_time.seconds,
                     current_time.date,
                     current_time.month,
                     current_time.year);
        }
        else
        {
            ESP_LOGE(TAG, "Failed to read time");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}





