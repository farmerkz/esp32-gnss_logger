/**
 * @file app_wifi.h
 * @brief Модуль управления инициализацией WiFi и сетевым интерфейсом esp_netif.
 */

#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация сетевого стека NVS, esp_netif и драйвера WiFi.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_wifi_init(void);

/**
 * @brief Подключение к целевой WiFi точке доступа для передачи файлов на FTP.
 * @param ssid Имя сети (SSID).
 * @param password Пароль к сети.
 * @param timeout_ms Максимальное время ожидания подключения в мс.
 * @return esp_err_t ESP_OK при успешном получении IP адреса.
 */
esp_err_t app_wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Проверка текущего состояния подключения WiFi.
 * @return true если подключён и имеет IP-адрес.
 */
bool app_wifi_is_connected(void);

/**
 * @brief Отключение от WiFi точки доступа.
 */
void app_wifi_disconnect_sta(void);

/**
 * @brief Проверяет, завершились ли автоматические попытки переподключения неудачей.
 * @return true если попытки исчерпаны (или ещё не начинались с момента загрузки).
 */
bool app_wifi_is_failed(void);

#ifdef __cplusplus
}
#endif

#endif // APP_WIFI_H
