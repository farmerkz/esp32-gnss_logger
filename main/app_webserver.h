/**
 * @file app_webserver.h
 * @brief Модуль встраиваемого веб-сервера для управления устройством, просмотра логов и OTA обновления.
 */

#ifndef APP_WEBSERVER_H
#define APP_WEBSERVER_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Структура состояния проверок оборудования системы (Чек-лист)
 */
typedef struct {
    bool nvs_config_ok;
    bool sd_card_ok;
    bool gnss_ok;
    bool wifi_ok;
} system_checklist_t;

extern system_checklist_t g_system_checklist;

/**
 * @brief Запуск HTTP веб-сервера (если включен в конфигурации и заданы пользователь/пароль).
 * @return esp_err_t ESP_OK при успешном старте.
 */
esp_err_t app_webserver_start(void);

/**
 * @brief Остановка HTTP веб-сервера.
 */
void app_webserver_stop(void);

/**
 * @brief Проверка, запущен ли веб-сервер в данный момент.
 * @return true если веб-сервер активен.
 */
bool app_webserver_is_running(void);

/**
 * @brief Проверка, выполняются ли в конфигурации условия для активации веб-сервера.
 * @return true если webserver_enable == true и заполнены логин и пароль.
 */
bool app_webserver_is_enabled_in_config(void);

#ifdef __cplusplus
}
#endif

#endif // APP_WEBSERVER_H
