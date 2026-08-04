/**
 * @file app_log_buffer.h
 * @brief Модуль перехвата и кольцевого буферизованного хранения системных логов в DRAM.
 */

#ifndef APP_LOG_BUFFER_H
#define APP_LOG_BUFFER_H

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_BUFFER_SIZE (16 * 1024) // 16 КБ кольцевой буфер логов

/**
 * @brief Инициализация перехватчика сообщений ESP_LOG в кольцевой буфер.
 */
void app_log_buffer_init(void);

/**
 * @brief Чтение накопленного системного лога.
 * @param buffer Выходной буфер для сохранения логов.
 * @param max_len Максимальный размер выходного буфера.
 * @return Фактическое количество скопированных байт лога.
 */
size_t app_log_buffer_get(char *buffer, size_t max_len);

/**
 * @brief Чтение накопленного лога для сохранения на SD-карту.
 * Продвигает внутренний указатель s_sd_tail.
 * @param buffer Выходной буфер для сохранения логов.
 * @param max_len Максимальный размер выходного буфера.
 * @return Фактическое количество прочитанных байт лога.
 */
size_t app_log_buffer_read_for_sd(char *buffer, size_t max_len);

/**
 * @brief Очистка буфера системного лога.
 */
void app_log_buffer_clear(void);

#ifdef __cplusplus
}
#endif

#endif // APP_LOG_BUFFER_H
