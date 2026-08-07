/**
 * @file app_wifi.h
 * @brief Модуль управления инициализацией WiFi и сетевым интерфейсом esp_netif (FSM).
 */

#ifndef APP_WIFI_H
#define APP_WIFI_H

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

extern EventGroupHandle_t g_network_event_group;

#define BIT_WIFI_CONNECTED BIT0
#define BIT_AP_AVAILABLE   BIT1
#define BIT_WIFI_FAILED    BIT2

/**
 * @brief Инициализация сетевого стека NVS, esp_netif, драйвера WiFi и запуск FSM задачи.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_wifi_init(void);

/**
 * @brief Проверка текущего состояния подключения WiFi.
 * @return true если подключён и имеет IP-адрес.
 */
bool app_wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif // APP_WIFI_H
