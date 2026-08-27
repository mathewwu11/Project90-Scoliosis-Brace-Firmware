#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "MCP3208";

// ---- Pin configuration (change to match your wiring) ----
#define PIN_NUM_MISO 12   // MCP3208 DOUT
#define PIN_NUM_MOSI 11   // MCP3208 DIN
#define PIN_NUM_CLK  13   // MCP3208 CLK
#define PIN_NUM_CS   10    // MCP3208 CS/SHDN

#define SPI_HOST_USED SPI2_HOST   // VSPI on most ESP32 dev boards
#define ADC_CHANNEL   0           // channel we want to read

static spi_device_handle_t spi_handle;

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

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing SPI for MCP3208...");
    mcp3208_spi_init();

    while (1) {
        uint16_t raw = mcp3208_read_channel(spi_handle, ADC_CHANNEL);
        float voltage = (raw / 4095.0f) * 3.3f; // assumes VREF = 3.3V

        ESP_LOGI(TAG, "CH%d raw: %4d  voltage: %.3f V", ADC_CHANNEL, raw, voltage);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
