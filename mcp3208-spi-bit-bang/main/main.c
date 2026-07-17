#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define SELPIN     GPIO_NUM_10
#define DATAOUT    GPIO_NUM_11
#define DATAIN     GPIO_NUM_12
#define SPICLOCK   GPIO_NUM_13

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

void app_main(void)
{
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

    while (1)
    {
        int readvalue;
        // Pin 1 on MCP3208
        readvalue = read_adc(0);
        printf("Channel 0\n");
        printf("%d\n", readvalue);
        // Pin 2 on MCP3208
        readvalue = read_adc(1);
        printf("Channel 1\n");
        printf("%d\n\n", readvalue);

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}