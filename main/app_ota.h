/**
 * @file app_ota.h
 * @brief Модуль безопасного обновления прошивки устройства с SD-карты с помощью esp_ota_ops.
 */

#ifndef APP_OTA_H
#define APP_OTA_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_FIRMWARE_PATH_1 "/sdcard/firmware.bin"
#define OTA_FIRMWARE_PATH_2 "/sdcard/esp32-gnss_logger.bin"

typedef enum {
    APP_OTA_SRC_NONE = 0,
    APP_OTA_SRC_SD,
    APP_OTA_SRC_WEB
} app_ota_source_t;

typedef struct {
    uint32_t magic;           // 0x07A00002
    uint8_t  source;          // app_ota_source_t
    bool     download_ok;     // Успешность загрузки / записи образа
    bool     first_boot_ok;   // Успешность первого запуска новой версии
    bool     pending_verify;  // Флаг ожидания проверки первого запуска
    char     error_msg[64];   // Описание ошибки
} app_ota_status_t;

/**
 * @brief Инициализация OTA модуля при старте приложения и отслеживание автоотката Rollback.
 */
esp_err_t app_ota_init(void);

/**
 * @brief Сохранение статуса OTA в NVS Flash.
 */
esp_err_t app_ota_save_status(app_ota_source_t src, bool download_ok, bool first_boot_ok, bool pending_verify, const char *err_msg);

/**
 * @brief Получение последнего сохраненного статуса OTA из NVS Flash.
 */
esp_err_t app_ota_get_status(app_ota_status_t *out_status);

/**
 * @brief Проверка наличия файла firmware.bin на SD карте и запуск обновления OTA при обнаружении.
 * @return esp_err_t ESP_OK если прошивка не требовалась или успешно произведена.
 */
esp_err_t app_ota_check_and_update(void);

/**
 * @brief Подтверждение успешного старта и валидности текущей прошивки (отмена механизма автоотката Rollback).
 * Вызывается после успешного запуска всех подсистем приложения.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_ota_mark_valid(void);

/**
 * @brief Пометка текущей прошивки как невалидной и мгновенный откат (Rollback) на предыдущую рабочую версию.
 * Вызывается при неустранимых фатальных сбоях на этапе старта новой версии.
 */
void app_ota_mark_invalid_and_reboot(void);

#ifdef __cplusplus
}
#endif

#endif // APP_OTA_H
