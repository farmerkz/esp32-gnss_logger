/**
 * @file app_gpx.h
 * @brief Модуль безопасной форматированной записи GPX трека и восстановления битых файлов при отключении питания.
 */

#ifndef APP_GPX_H
#define APP_GPX_H

#include <stdbool.h>
#include "esp_err.h"
#include "ubx_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация задачи записи GPX треков.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_gpx_init(void);

/**
 * @brief Безопасное восстановление формата GPX файла после внезапного сбоя питания
 * и корректная дозапись завершающих тегов GPX перед перегрузкой в готовность .rd.
 * @param filepath Полный путь к файлу трека.
 * @return esp_err_t ESP_OK при успешной валидации и закрытии.
 */
esp_err_t app_gpx_repair_and_close(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif // APP_GPX_H
