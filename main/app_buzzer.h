/**
 * @file app_buzzer.h
 * @brief Модуль звуковой сигнализации (Buzzer) на базе ESP-IDF LEDC периферии.
 */

#ifndef APP_BUZZER_H
#define APP_BUZZER_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @brief Мьютекс для сериализации доступа к LEDC-драйверу пищалки.
 *        Захватывается внутри app_buzzer_play() автоматически.
 */
extern SemaphoreHandle_t g_buzzer_mutex;

#ifdef __cplusplus
extern "C" {
#endif

// GPIO пин пищалки (по умолчанию GPIO 25)
#define BUZZER_GPIO_PIN 25

/**
 * @brief Типы звуковых уведомлений системы
 */
typedef enum {
    APP_BUZZER_STARTUP = 0, // Старт системы
    APP_BUZZER_GNSS_FIX,   // Получен валидный 3D GNSS Fix
    APP_BUZZER_GNSS_LOST,  // Потерян 3D GNSS Fix
    APP_BUZZER_FILE_ROTATE,// Ротация файлов на SD
    APP_BUZZER_FTP_SUCCESS,// Успешная отправка файлов на FTP
    APP_BUZZER_FTP_ERROR,  // Ошибка отправки на FTP
    APP_BUZZER_FATAL       // Фатальная ошибка системы
} app_buzzer_sound_t;

/**
 * @brief Инициализация периферии LEDC для управления пищалкой.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_buzzer_init(void);

/**
 * @brief Проигрывание одиночного звукового тона заданной частоты и длительности.
 * @param freq_hz Частота тона в Гц (0 для выключения).
 * @param duration_ms Длительность звучания в мс.
 */
void app_buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief Проигрывание системного паттерна звуковой сигнализации.
 * @param sound Тип звукового сигнала из app_buzzer_sound_t.
 */
void app_buzzer_play(app_buzzer_sound_t sound);

#ifdef __cplusplus
}
#endif

#endif // APP_BUZZER_H
