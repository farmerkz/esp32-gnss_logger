/**
 * @file app_log_buffer.c
 * @brief Реализация кольцевого буфера системного лога на базе esp_log_set_vprintf.
 */

#include "app_log_buffer.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_log.h"
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_head = 0;
static size_t s_tail = 0;
static bool s_full = false;

static size_t s_sd_tail = 0;
static bool s_sd_empty = true;

static SemaphoreHandle_t s_log_mutex = NULL;
static vprintf_like_t s_prev_vprintf = NULL;

static int call_prev_vprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
    va_end(args);
    return ret;
}

static int custom_vprintf(const char *fmt, va_list args)
{
    // Форматируем исходную строку лога
    char temp[256];
    int len = vsnprintf(temp, sizeof(temp), fmt, args);
    if (len <= 0) return 0;
    if (len >= sizeof(temp)) len = sizeof(temp) - 1;

    // Получаем текущее время
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char final_buf[300];
    int final_len = 0;

    // Если год > 2020, значит время синхронизировано (GNSS)
    if (timeinfo.tm_year > (2020 - 1900)) {
        final_len = snprintf(final_buf, sizeof(final_buf), "[%04d-%02d-%02d %02d:%02d:%02d] %s",
                             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, temp);
    } else {
        // Если время не синхронизировано, оставляем как есть
        final_len = snprintf(final_buf, sizeof(final_buf), "%s", temp);
    }

    if (final_len >= sizeof(final_buf)) final_len = sizeof(final_buf) - 1;

    // Выводим в консоль
    int ret = call_prev_vprintf("%s", final_buf);

    // Записываем в кольцевой буфер (включая дату и время)
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        for (int i = 0; i < final_len; i++) {
            s_log_buffer[s_head] = final_buf[i];
            s_head = (s_head + 1) % LOG_BUFFER_SIZE;
            s_sd_empty = false;
            if (s_full) {
                if (s_sd_tail == s_tail) {
                    s_sd_tail = (s_sd_tail + 1) % LOG_BUFFER_SIZE;
                }
                s_tail = (s_tail + 1) % LOG_BUFFER_SIZE;
            }
            if (s_head == s_tail) {
                s_full = true;
            }
        }
        xSemaphoreGive(s_log_mutex);
    }

    return ret;
}

void app_log_buffer_init(void)
{
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutex();
    }
    s_head = 0;
    s_tail = 0;
    s_full = false;
    s_sd_tail = 0;
    s_sd_empty = true;
    memset(s_log_buffer, 0, sizeof(s_log_buffer));

    s_prev_vprintf = esp_log_set_vprintf(custom_vprintf);
}

size_t app_log_buffer_get(char *buffer, size_t max_len)
{
    if (buffer == NULL || max_len == 0) {
        return 0;
    }

    size_t bytes_copied = 0;
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        size_t count = s_full ? LOG_BUFFER_SIZE : ((s_head >= s_tail) ? (s_head - s_tail) : (LOG_BUFFER_SIZE - s_tail + s_head));
        if (count >= max_len) {
            count = max_len - 1;
        }

        size_t idx = s_full ? s_head : s_tail; // При переполнении начинаем с самого старого элемента
        for (size_t i = 0; i < count; i++) {
            buffer[i] = s_log_buffer[(idx + i) % LOG_BUFFER_SIZE];
        }
        buffer[count] = '\0';
        bytes_copied = count;

        xSemaphoreGive(s_log_mutex);
    }

    return bytes_copied;
}

size_t app_log_buffer_read_for_sd(char *buffer, size_t max_len)
{
    if (buffer == NULL || max_len == 0) {
        return 0;
    }

    size_t bytes_copied = 0;
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!s_sd_empty) {
            size_t count = (s_head > s_sd_tail) ? (s_head - s_sd_tail) : (LOG_BUFFER_SIZE - s_sd_tail + s_head);
            if (count > max_len) {
                count = max_len;
            }

            for (size_t i = 0; i < count; i++) {
                buffer[i] = s_log_buffer[(s_sd_tail + i) % LOG_BUFFER_SIZE];
            }
            
            s_sd_tail = (s_sd_tail + count) % LOG_BUFFER_SIZE;
            if (s_sd_tail == s_head) {
                s_sd_empty = true;
            }
            bytes_copied = count;
        }
        xSemaphoreGive(s_log_mutex);
    }
    return bytes_copied;
}

void app_log_buffer_clear(void)
{
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_head = 0;
        s_tail = 0;
        s_full = false;
        s_sd_tail = 0;
        s_sd_empty = true;
        memset(s_log_buffer, 0, sizeof(s_log_buffer));
        xSemaphoreGive(s_log_mutex);
    }
}
