/**
 * @file app_gnss.h
 * @brief Модуль автоопределения скорости UART, инициализации и приема данных от GNSS модуля.
 */

#ifndef APP_GNSS_H
#define APP_GNSS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "ubx_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_UART_NUM       UART_NUM_2
#define GNSS_TX_GPIO_PIN    17
#define GNSS_RX_GPIO_PIN    16
#define GNSS_TARGET_BAUD    115200

/**
 * @brief Инициализация GNSS модуля: автоопределение скорости UART, переключение на 115200 бод,
 * верификация и конфигурация динамической модели и масок.
 * @return esp_err_t ESP_OK при успешном выходе на 115200 бод и инициализации.
 */
esp_err_t app_gnss_init(void);

/**
 * @brief Получение актуальных принятых данных PVT.
 * @param pvt Выходной указатель на структуру ubx_nav_pvt_t.
 * @return true если данные доступны и валидны.
 */
bool app_gnss_get_latest_pvt(ubx_nav_pvt_t *pvt);

/**
 * @brief Проверка валидности текущей 3D фиксации GNSS.
 * @return true если зафиксирована 3D позиция и gnssFixOK == 1.
 */
bool app_gnss_is_fix_valid(void);

/**
 * @brief Получение последних принятых данных DOP (Dilution of Precision).
 * @param dop Выходной указатель на структуру ubx_nav_dop_t.
 * @return true если данные DOP доступны (был принят хотя бы один пакет NAV-DOP).
 */
bool app_gnss_get_latest_dop(ubx_nav_dop_t *dop);

#ifdef __cplusplus
}
#endif

#endif // APP_GNSS_H
