/**
 * @file app_sdcard.c
 * @brief Реализация работы с SD картой по SPI интерфейсу в ESP-IDF VFS FAT.
 */

#include "app_sdcard.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "app_gpx.h"
#include "app_wigle.h"
#include "app_config.h"

static const char *TAG = "SDCARD";

SemaphoreHandle_t g_sd_mutex = NULL;

// Назначение пинов SPI по умолчанию для ESP32 DevKit (VSPI)
#define PIN_NUM_MISO 19
#define PIN_NUM_MOSI 23
#define PIN_NUM_CLK  18
#define PIN_NUM_CS   5

static sdmmc_card_t *s_card = NULL;

esp_err_t app_sdcard_ensure_dir(const char *path)
{
    struct stat st;

    /* stat + mkdir выполняем в ОДНОМ захвате мьютекса, исключая гонку между проверкой и созданием */
    if (g_sd_mutex) xSemaphoreTake(g_sd_mutex, portMAX_DELAY);

    bool dir_exists = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    if (dir_exists) {
        if (g_sd_mutex) xSemaphoreGive(g_sd_mutex);
        return ESP_OK;
    }

    bool created = (mkdir(path, 0775) == 0);
    if (g_sd_mutex) xSemaphoreGive(g_sd_mutex);

    if (created) {
        ESP_LOGI(TAG, "Created directory: %s", path);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Failed to create directory: %s", path);
    return ESP_FAIL;
}

esp_err_t app_sdcard_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card via SPI...");

    g_sd_mutex = xSemaphoreCreateMutex();
    if (g_sd_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create SD mutex!");
        return ESP_ERR_NO_MEM;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 8,   /* GPX + Wigle CSV + syslog + FTP read + config + webserver + запас */
        .allocation_unit_size = 16 * 1024
    };

    // Подтяжка MISO к питанию обязательна в SPI-режиме SD-карты:
    // без pull-up линия MISO флоатит при ответе карты — возникает ESP_ERR_TIMEOUT
    gpio_set_pull_mode(PIN_NUM_MISO, GPIO_PULLUP_ONLY);

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = SPI2_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384, // 16 КБ — достаточно для SPI DMA с SD
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    // Снижаем частоту SPI: 20 МГц по умолчанию вызывает таймаут на ACMD51 (SCR).
    // 4 МГц — надёжное значение для большинства карт и схем с SPI
    host.max_freq_khz = 4000;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD filesystem: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD Card mounted successfully!");
    sdmmc_card_print_info(stdout, s_card);

    // Подготовка всех небходимых директорий
    app_sdcard_ensure_dir(DIR_GPS_WK);
    app_sdcard_ensure_dir(DIR_GPS_RD);
    app_sdcard_ensure_dir(DIR_GPS_SD);
    app_sdcard_ensure_dir(DIR_WIFI_WK);
    app_sdcard_ensure_dir(DIR_WIFI_RD);
    app_sdcard_ensure_dir(DIR_WIFI_SD);

    return ESP_OK;
}

void app_sdcard_cleanup_work_dirs(void)
{
    DIR *dir;
    struct dirent *entry;
    char src_path[512];
    char dst_path[512];

    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);

    // 1. Очистка каталога GPS
    dir = opendir(DIR_GPS_WK);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                snprintf(src_path, sizeof(src_path), "%s/%s", DIR_GPS_WK, entry->d_name);
                struct stat st;
                /* P-4: используем конфигурируемый порог вместо константы 500 */
                uint32_t min_size = g_app_config.min_track_size;
                if (min_size == 0) min_size = 1000;
                if (stat(src_path, &st) == 0 && (uint32_t)st.st_size < min_size) {
                    /* Удаляем мелкий битый файл (слишком мало точек) */
                    unlink(src_path);
                    ESP_LOGI(TAG, "Deleted small GPS file (%ld bytes < %lu): %s",
                             (long)st.st_size, (unsigned long)min_size, entry->d_name);
                } else {
                    /* Закрываем трек валидно перед переносом */
                    app_gpx_repair_and_close(src_path);
                    snprintf(dst_path, sizeof(dst_path), "%s/%s", DIR_GPS_RD, entry->d_name);
                    rename(src_path, dst_path);
                    ESP_LOGI(TAG, "Moved leftover GPS file to ready: %s", entry->d_name);
                }
            }
        }
        closedir(dir);
    }

    // 2. Очистка каталога WiFi
    dir = opendir(DIR_WIFI_WK);
    if (dir) {
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                snprintf(src_path, sizeof(src_path), "%s/%s", DIR_WIFI_WK, entry->d_name);
                struct stat st;
                /* P-4: используем конфигурируемый порог */
                uint32_t min_size = g_app_config.min_track_size;
                if (min_size == 0) min_size = 1000;
                if (stat(src_path, &st) == 0 && (uint32_t)st.st_size < min_size) {
                    unlink(src_path);
                    ESP_LOGI(TAG, "Deleted small WiFi file (%ld bytes < %lu): %s",
                             (long)st.st_size, (unsigned long)min_size, entry->d_name);
                } else {
                    /* W-1: ремонт последней строки CSV перед переносом */
                    esp_err_t repair_ret = app_wigle_repair_csv(src_path);
                    if (repair_ret != ESP_OK) {
                        ESP_LOGW(TAG, "CSV repair failed for '%s', moving anyway", entry->d_name);
                    }
                    snprintf(dst_path, sizeof(dst_path), "%s/%s", DIR_WIFI_RD, entry->d_name);
                    rename(src_path, dst_path);
                    ESP_LOGI(TAG, "Moved leftover WiFi file to ready: %s", entry->d_name);
                }
            }
        }
        closedir(dir);
    }

    xSemaphoreGive(g_sd_mutex);
}
