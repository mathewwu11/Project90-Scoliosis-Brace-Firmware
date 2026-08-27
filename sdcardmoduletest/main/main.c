#include <stdio.h>
#include <string.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <sys/stat.h>

static const char *TAG = "SD_TEST";

// ---- Pin definitions: EDIT to match your wiring ----
#define PIN_NUM_MISO  12
#define PIN_NUM_MOSI  11
#define PIN_NUM_CLK   13
#define PIN_NUM_CS    9

#define MOUNT_POINT "/sdcard"

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "=== ESP-IDF SD Card SPI Test ===");

    // ---------- 1. Mount config ----------
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false, // set true only if you want to auto-format blank cards
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;

    ESP_LOGI(TAG, "Initializing SPI bus...");

    // ---------- 2. SPI bus config ----------
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI bus initialized OK.");

    // ---------- 3. SD SPI device config ----------
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 400; // start slow (400kHz) for reliability, can raise later

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    // ---------- 4. Mount filesystem (this is where SPI comms + card init happens) ----------
    ESP_LOGI(TAG, "Mounting filesystem...");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                          "If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                          "Check wiring, CS pin, pull-up resistors, and power.", esp_err_to_name(ret));
        }
        spi_bus_free(SPI2_HOST);
        return;
    }
    ESP_LOGI(TAG, "Filesystem mounted OK.");

    // ---------- 5. Confirm communication: print card info ----------
    sdmmc_card_print_info(stdout, card);

    // ---------- 6. File management + write/read/verify test ----------
    const char *test_path = MOUNT_POINT "/test.txt";

    ESP_LOGI(TAG, "Opening file for writing: %s", test_path);
    FILE *f = fopen(test_path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
        goto cleanup;
    }
    fprintf(f, "Hello ESP32-S2, this is a write test.\n");
    fclose(f);
    ESP_LOGI(TAG, "Write successful.");

    // Append test
    ESP_LOGI(TAG, "Appending to file...");
    f = fopen(test_path, "a");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for appending");
        goto cleanup;
    }
    fprintf(f, "This line was appended.\n");
    fclose(f);
    ESP_LOGI(TAG, "Append successful.");

    // Read + verify
    ESP_LOGI(TAG, "Reading file back...");
    f = fopen(test_path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading");
        goto cleanup;
    }

    char line[128];
    bool found_hello = false, found_appended = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("Read line: %s", line);
        if (strstr(line, "Hello ESP32-S2") != NULL) found_hello = true;
        if (strstr(line, "appended") != NULL) found_appended = true;
    }
    fclose(f);

    if (found_hello && found_appended) {
        ESP_LOGI(TAG, "READ/WRITE VERIFICATION PASSED");
    } else {
        ESP_LOGE(TAG, "READ/WRITE VERIFICATION FAILED");
    }

    // ---------- 7. Directory creation test (file management) ----------
    const char *dir_path = MOUNT_POINT "/logs";
    struct stat st;
    if (stat(dir_path, &st) != 0) {
        if (mkdir(dir_path, 0755) == 0) {
            ESP_LOGI(TAG, "Created directory: %s", dir_path);
        } else {
            ESP_LOGE(TAG, "mkdir failed");
        }
    } else {
        ESP_LOGI(TAG, "Directory already exists: %s", dir_path);
    }

    ESP_LOGI(TAG, "=== Test complete ===");

cleanup:
    // ---------- 8. Unmount and free bus (good practice, esp. before power-cycling card) ----------
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted.");
    spi_bus_free(SPI2_HOST);
}