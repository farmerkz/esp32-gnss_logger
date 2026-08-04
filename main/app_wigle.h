/**
 * @file app_wigle.h
 * @brief Модуль сканирования WiFi эфира и сохранения результатов в формате Wigle CSV v1.4.
 */

#ifndef APP_WIGLE_H
#define APP_WIGLE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация фоновой задачи сканирования WiFi и логирования Wigle CSV.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_wigle_init(void);

/**
 * @brief Проверяет и восстанавливает последнюю строку Wigle CSV файла.
 *        Выполняет обратный поиск по символу \n с проверкой формата (11 полей CSV).
 *        Должна вызываться под мьютексом g_sd_mutex.
 * @param filepath Полный путь к файлу CSV.
 * @return esp_err_t ESP_OK при успехе, ESP_FAIL при неустранимом повреждении.
 */
esp_err_t app_wigle_repair_csv(const char *filepath);

#ifdef __cplusplus
}
#endif

#endif // APP_WIGLE_H
