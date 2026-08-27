#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define ADC_CHANNEL ADC_CHANNEL_0   // GPIO1 on ESP32-S2
#define ADC_SAMPLES 64

static const char *TAG = "ADC_TEST";

adc_oneshot_unit_handle_t adc_handle;


// Read ADC with averaging
int read_adc_average()
{
    int sum = 0;

    for (int i = 0; i < ADC_SAMPLES; i++)
    {
        int raw;

        ESP_ERROR_CHECK(
            adc_oneshot_read(
                adc_handle,
                ADC_CHANNEL,
                &raw
            )
        );

        sum += raw;

        // Small delay between samples
        esp_rom_delay_us(100);
    }

    return sum / ADC_SAMPLES;
}


void app_main(void)
{
    // Initialize ADC unit
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_new_unit(
            &init_config,
            &adc_handle
        )
    );


    // Configure ADC channel
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(
        adc_oneshot_config_channel(
            adc_handle,
            ADC_CHANNEL,
            &chan_config
        )
    );


    while (1)
    {
        int adc_raw = read_adc_average();

        // ESP32-S2 ADC appears to be using 13-bit output
        float voltage = ((float)adc_raw / 8191.0f) * 3.3f;


        ESP_LOGI(
            TAG,
            "Raw: %d   Voltage: %.3f V",
            adc_raw,
            voltage
        );


        vTaskDelay(pdMS_TO_TICKS(500));
    }
}