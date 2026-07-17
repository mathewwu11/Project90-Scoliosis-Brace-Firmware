#include <stdio.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#define PIN_NUM_MISO   37
#define PIN_NUM_MOSI   35
#define PIN_NUM_CLK    36
#define PIN_NUM_CS     34
#define PIN_NUM_DRDY    4

#define SPI_HOST SPI2_HOST
#define AD7714_MCLK_GPIO GPIO_NUM_25

static spi_device_handle_t ad7714_spi;
static SemaphoreHandle_t drdy_sem;

static esp_err_t ad7714_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t tx[2];

    tx[0] = reg;
    tx[1] = value;

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
    };

    return spi_device_transmit(ad7714_spi, &t);
}

static uint8_t ad7714_read_reg(uint8_t reg)
{
    uint8_t tx[2] = {reg | 0x40, 0};
    uint8_t rx[2];

    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    spi_device_transmit(ad7714_spi, &t);

    return rx[1];
}

static int32_t ad7714_read_data(void)
{
    uint8_t tx[4] = {
        0x45,      // Read Data Register
        0,
        0,
        0
    };

    uint8_t rx[4];

    spi_transaction_t t = {
        .length = 32,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    spi_device_transmit(ad7714_spi, &t);

    return ((int32_t)rx[1] << 16) |
           ((int32_t)rx[2] << 8)  |
            rx[3];
}

static void ad7714_reset(void)
{
    uint8_t tx[4] = {
        0xFF,
        0xFF,
        0xFF,
        0xFF
    };

    spi_transaction_t t = {
        .length = 32,
        .tx_buffer = tx,
    };

    spi_device_transmit(ad7714_spi, &t);

    vTaskDelay(pdMS_TO_TICKS(5));
}

static void ad7714_configure(void)
{
    // Setup register
    ad7714_write_reg(0x10, 0x40);

    // Mode register
    ad7714_write_reg(0x20, 0x02);

    // Filter register
    ad7714_write_reg(0x30, 0x07);
}

static void IRAM_ATTR drdy_isr(void *arg)
{
    BaseType_t high_task = pdFALSE;

    xSemaphoreGiveFromISR(drdy_sem, &high_task);

    if (high_task)
        portYIELD_FROM_ISR();
}

static void spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 1000000,
        .mode = 3,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
    };

    spi_bus_add_device(SPI_HOST, &devcfg, &ad7714_spi);
}

static void drdy_init(void)
{
    drdy_sem = xSemaphoreCreateBinary();

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_NUM_DRDY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    gpio_config(&io);

    gpio_install_isr_service(0);

    gpio_isr_handler_add(PIN_NUM_DRDY,
                         drdy_isr,
                         NULL);
}

static void adc_task(void *arg)
{
    while (1)
    {
        xSemaphoreTake(drdy_sem, portMAX_DELAY);

        int32_t adc = ad7714_read_data();

        float voltage =
            (float)adc * 5.0f / 16777215.0f;

        printf("ADC: %ld   Voltage: %.6f V\n",
               (long)adc,
               voltage);
    }
}

static void ad7714_mclk_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_1_BIT,
        .freq_hz = 1000000,           // 1 MHz
        .clk_cfg = LEDC_AUTO_CLK,
    };

    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .gpio_num = AD7714_MCLK_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 1,                    // 50% duty cycle
        .hpoint = 0,
    };

    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

void app_main(void)
{
    // Start the AD7714 master clock
    ad7714_mclk_init();

    // Give the clock a moment to stabilize
    vTaskDelay(pdMS_TO_TICKS(10));
    
    spi_init();

    drdy_init();

    ad7714_reset();

    ad7714_configure();

    xTaskCreate(adc_task,
                "adc_task",
                4096,
                NULL,
                5,
                NULL);
}
