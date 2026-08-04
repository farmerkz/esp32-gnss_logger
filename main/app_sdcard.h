/**
 * @file app_sdcard.h
 * @brief Модуль монтирования SD-карты, управления структурой папок и мьютексом VFS.
 */

#ifndef APP_SDCARD_H
#define APP_SDCARD_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"

#define DIR_GPS_WK  "/sdcard/gps.wk"
#define DIR_GPS_RD  "/sdcard/gps.rd"
#define DIR_GPS_SD  "/sdcard/gps.sd"

#define DIR_WIFI_WK "/sdcard/wifi.wk"
#define DIR_WIFI_RD "/sdcard/wifi.rd"
#define DIR_WIFI_SD "/sdcard/wifi.sd"

/**
 * @brief Мьютекс потокобезопасного доступа к VFS файловой системы SD-карты
 */
extern SemaphoreHandle_t g_sd_mutex;

/**
 * @brief Инициализация и монтирование SD-карты по VFS SPI протоколу.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_sdcard_init(void);

/**
 * @brief Проверка существования каталога и его создание при отсутствии.
 * @param path Полный путь к папке.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_sdcard_ensure_dir(const char *path);

/**
 * @brief Проверка рабочих каталогов .wk и перемещение оставшихся файлов в .rd при старте.
 */
void app_sdcard_cleanup_work_dirs(void);

#ifdef __cplusplus
}
#endif

#endif // APP_SDCARD_H
