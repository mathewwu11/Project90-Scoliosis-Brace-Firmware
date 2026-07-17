#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

#include "driver/spi_common.h"
#include "driver/spi_master.h"

#include "sdmmc_cmd.h"

static const char *TAG = "SD_CARD";

// ===== PIN CONFIG =====
#define PIN_NUM_MISO 12
#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  13
#define PIN_NUM_CS   9

#define MOUNT_POINT "/sdcard"
#define FILE_PATH   MOUNT_POINT "/log.txt"

void write_to_sd(const char *text)
{
    FILE *file = fopen(FILE_PATH, "a");

    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open file");
        return;
    }

    fprintf(file, "%s\n", text);

    fclose(file);

    ESP_LOGI(TAG, "Data written successfully");
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing SD card");

    // SPI bus config
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI init failed");
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t *card;

    ret = esp_vfs_fat_sdspi_mount(
        MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card");
        return;
    }

    ESP_LOGI(TAG, "SD card mounted");

    // Example write
    write_to_sd("ESP32 boot complete");

    int counter = 0;

    while (1) {
        char buffer[64];

        snprintf(
            buffer,
            sizeof(buffer),
            "Timestamp: %lu Count: %d",
            (unsigned long)esp_log_timestamp(),
            counter++
        );

        write_to_sd(buffer);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    // Optional cleanup
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
}