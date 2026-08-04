/**
 * @file app_gnss.c
 * @brief Реализация автоопределения скорости UART, переключения на 115200 бод и обработки UBX.
 */

#include "app_gnss.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "app_config.h"
#include "app_buzzer.h"

static const char *TAG = "GNSS";

static ubx_parser_t    s_ubx_parser;
static ubx_nav_pvt_t   s_latest_pvt;
static ubx_nav_dop_t   s_latest_dop;
static bool            s_has_pvt      = false;
static bool            s_has_dop      = false;
static bool            s_prev_fix_state = false;
static SemaphoreHandle_t s_pvt_mutex  = NULL;

static const uint32_t s_baud_rates[] = {115200, 9600, 38400, 57600, 19200};
#define NUM_BAUD_RATES (sizeof(s_baud_rates) / sizeof(s_baud_rates[0]))

/**
 * @brief Зондирование скорости UART GNSS-модуля.
 *
 * Устанавливает скорость UART, сбрасывает буфер приёма, отправляет UBX-MON-VER Poll
 * и ждёт ответа до 700 мс. Определяет как UBX-ответ, так и NMEA-поток (символ '$').
 *
 * @param baud              Скорость для проверки.
 * @param out_ubx_detected  Выходной флаг: true — пришёл валидный UBX-пакет,
 *                          false — только NMEA или ничего.
 * @return true если скорость определена (есть хоть какой-то сигнал от модуля).
 */
static bool app_gnss_probe_baud(uint32_t baud, bool *out_ubx_detected)
{
    ESP_LOGI(TAG, "Probing GNSS UART at %lu baud...", baud);

    if (out_ubx_detected != NULL) {
        *out_ubx_detected = false;
    }

    uart_config_t uart_config = {
        .baud_rate  = baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(GNSS_UART_NUM, &uart_config);

    // Очищаем буфер приема UART
    uart_flush_input(GNSS_UART_NUM);

    // Формируем запрос версии UBX-MON-VER (Poll)
    uint8_t poll_cmd[16];
    size_t cmd_len = ubx_build_mon_ver_poll(poll_cmd);

    ubx_parser_init(&s_ubx_parser);
    uart_write_bytes(GNSS_UART_NUM, (const char *)poll_cmd, cmd_len);

    uint8_t rx_buf[128];
    int rx_len = 0;
    bool nmea_detected = false;
    TickType_t start_tick = xTaskGetTickCount();

    // Ожидаем ответа до 700 мс:
    // - UBX-ответ (MON-VER, PVT, ACK, любой другой валидный пакет)
    // - ИЛИ NMEA-строка (символ '$') — признак совпадения скорости при NMEA-only режиме
    //   (заводские настройки u-blox: NMEA 9600 бод; предыдущая конфигурация могла сбросить модуль)
    while ((xTaskGetTickCount() - start_tick) < pdMS_TO_TICKS(700)) {
        rx_len = uart_read_bytes(GNSS_UART_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(20));
        if (rx_len > 0) {
            for (int i = 0; i < rx_len; i++) {
                // Детектирование NMEA: символ '$' означает начало NMEA-предложения
                if (rx_buf[i] == '$') {
                    nmea_detected = true;
                }
                ubx_parse_result_t res = ubx_parser_process_byte(&s_ubx_parser, rx_buf[i]);
                if (res == UBX_PARSE_GOT_MON_VER || res == UBX_PARSE_GOT_PVT ||
                    res == UBX_PARSE_GOT_ACK    || res == UBX_PARSE_GOT_OTHER) {
                    ESP_LOGI(TAG, "UBX response detected at %lu baud!", baud);
                    if (out_ubx_detected != NULL) {
                        *out_ubx_detected = true;
                    }
                    return true;
                }
            }
        }
    }

    // Если пришёл NMEA — скорость верная, но модуль в NMEA-режиме (UBX не обнаружен)
    if (nmea_detected) {
        ESP_LOGI(TAG, "NMEA data detected at %lu baud (module in NMEA-only mode).", baud);
        return true;
    }

    return false;
}

static void app_gnss_sync_system_time(const ubx_nav_pvt_t *pvt)
{
    static bool s_time_synced = false;
    if (s_time_synced) {
        return;
    }

    if ((pvt->valid & 0x07) == 0x07 && pvt->fixType == 3) {
        struct tm tm_info;
        memset(&tm_info, 0, sizeof(struct tm));
        tm_info.tm_year = pvt->year - 1900;
        tm_info.tm_mon  = pvt->month - 1;
        tm_info.tm_mday = pvt->day;
        tm_info.tm_hour = pvt->hour;
        tm_info.tm_min  = pvt->min;
        tm_info.tm_sec  = pvt->sec;
        tm_info.tm_isdst = 0;

        time_t t = mktime(&tm_info);
        // Коррекция смещения часового пояса из конфигурации (в секундах)
        t += ((time_t)g_app_config.timezone_offset * 3600);

        struct timeval tv = {
            .tv_sec  = t,
            .tv_usec = 0
        };
        settimeofday(&tv, NULL);
        s_time_synced = true;
        ESP_LOGI(TAG, "System time synchronized via GNSS UTC: %04d-%02d-%02d %02d:%02d:%02d (TZ: UTC%+d)",
                 pvt->year, pvt->month, pvt->day, pvt->hour, pvt->min, pvt->sec, g_app_config.timezone_offset);
    }
}

static void app_gnss_rx_task(void *pvParameters)
{
    uint8_t rx_buf[256];
    for (;;) {
        int rx_len = uart_read_bytes(GNSS_UART_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(50));
        if (rx_len > 0) {
            for (int i = 0; i < rx_len; i++) {
                ubx_parse_result_t res = ubx_parser_process_byte(&s_ubx_parser, rx_buf[i]);

                if (res == UBX_PARSE_GOT_PVT) {
                    xSemaphoreTake(s_pvt_mutex, portMAX_DELAY);

                    memcpy(&s_latest_pvt, &s_ubx_parser.last_pvt, sizeof(ubx_nav_pvt_t));
                    s_has_pvt = true;

                    bool current_fix = (s_latest_pvt.fixType == 3 && (s_latest_pvt.flags & 0x01));
                    if (current_fix != s_prev_fix_state) {
                        s_prev_fix_state = current_fix;
                        if (current_fix) {
                            ESP_LOGI(TAG, "GNSS 3D Fix ACQUIRED!");
                            app_buzzer_play(APP_BUZZER_GNSS_FIX);
                        } else {
                            ESP_LOGW(TAG, "GNSS 3D Fix LOST!");
                            app_buzzer_play(APP_BUZZER_GNSS_LOST);
                        }
                    }

                    app_gnss_sync_system_time(&s_latest_pvt);
                    xSemaphoreGive(s_pvt_mutex);

                } else if (res == UBX_PARSE_GOT_DOP) {
                    // Сохраняем DOP данные под тем же мьютексом (iTOW связывает их с PVT)
                    xSemaphoreTake(s_pvt_mutex, portMAX_DELAY);
                    memcpy(&s_latest_dop, &s_ubx_parser.last_dop, sizeof(ubx_nav_dop_t));
                    s_has_dop = true;
                    xSemaphoreGive(s_pvt_mutex);
                }
            }
        }
    }
}

esp_err_t app_gnss_init(void)
{
    ESP_LOGI(TAG, "Initializing GNSS UART driver...");

    s_pvt_mutex = xSemaphoreCreateMutex();

    uart_config_t default_uart_cfg = {
        .baud_rate  = GNSS_TARGET_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_driver_install(GNSS_UART_NUM, 1024 * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    uart_param_config(GNSS_UART_NUM, &default_uart_cfg);
    uart_set_pin(GNSS_UART_NUM, GNSS_TX_GPIO_PIN, GNSS_RX_GPIO_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // 1. Поиск текущей скорости UART модуля GNSS
    uint32_t detected_baud  = 0;
    bool     ubx_detected   = false;

    for (size_t i = 0; i < NUM_BAUD_RATES; i++) {
        if (app_gnss_probe_baud(s_baud_rates[i], &ubx_detected)) {
            detected_baud = s_baud_rates[i];
            break;
        }
    }

    if (detected_baud == 0) {
        ESP_LOGE(TAG, "Could not detect GNSS UART baudrate!");
        return ESP_ERR_NOT_FOUND;
    }

    // 2. Переключение скорости и/или вывода в UBX-режим через CFG-PRT.
    //
    // CFG-PRT отправляется ВСЕГДА в двух случаях:
    //   а) Скорость модуля отличается от целевой (115200) — меняем скорость + режим
    //   б) Скорость совпадает, но модуль ответил NMEA (не UBX) — переводим в UBX out-only
    //
    // Это гарантирует единую конфигурацию независимо от начального состояния модуля.
    if (detected_baud != GNSS_TARGET_BAUD) {
        ESP_LOGW(TAG, "GNSS is at %lu baud. Switching to %d baud with UBX output...",
                 detected_baud, GNSS_TARGET_BAUD);

        uint8_t cfg_prt_cmd[32];
        size_t  prt_len = ubx_build_cfg_prt_baud(cfg_prt_cmd, GNSS_TARGET_BAUD);
        uart_write_bytes(GNSS_UART_NUM, (const char *)cfg_prt_cmd, prt_len);
        vTaskDelay(pdMS_TO_TICKS(100));

        // Переключаем скорость на стороне ESP32
        uart_set_baudrate(GNSS_UART_NUM, GNSS_TARGET_BAUD);
        vTaskDelay(pdMS_TO_TICKS(100));

        // Верификация: проверяем наличие UBX-ответа на новой скорости
        if (!app_gnss_probe_baud(GNSS_TARGET_BAUD, &ubx_detected)) {
            ESP_LOGE(TAG, "Failed to verify connection at %d baud!", GNSS_TARGET_BAUD);
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "GNSS UART baudrate successfully switched to %d!", GNSS_TARGET_BAUD);

    } else if (!ubx_detected) {
        // Модуль уже на 115200, но выводит NMEA — переключаем в UBX out-only режим
        ESP_LOGW(TAG, "GNSS at target baud but in NMEA mode. Switching to UBX output...");

        uint8_t cfg_prt_cmd[32];
        size_t  prt_len = ubx_build_cfg_prt_baud(cfg_prt_cmd, GNSS_TARGET_BAUD);
        uart_write_bytes(GNSS_UART_NUM, (const char *)cfg_prt_cmd, prt_len);
        vTaskDelay(pdMS_TO_TICKS(150));

        // Повторная верификация: ожидаем UBX-ответ
        if (!app_gnss_probe_baud(GNSS_TARGET_BAUD, &ubx_detected) || !ubx_detected) {
            ESP_LOGE(TAG, "Failed to switch GNSS to UBX output mode!");
            return ESP_ERR_INVALID_RESPONSE;
        }
        ESP_LOGI(TAG, "GNSS successfully switched to UBX output mode.");

    } else {
        ESP_LOGI(TAG, "GNSS is at %d baud with UBX output. No reconfiguration needed.", GNSS_TARGET_BAUD);
    }

    // 3. Активация сообщений NAV-PVT и NAV-DOP через CFG-MSG.
    //
    // После перевода в UBX-режим модуль не отправляет навигационные сообщения
    // автоматически. Необходимо явно включить каждое нужное сообщение.
    uint8_t cfg_msg_cmd[16];
    size_t  msg_len;

    // Включаем NAV-PVT (0x01/0x07) — основной источник координат, времени и скорости
    msg_len = ubx_build_cfg_msg(cfg_msg_cmd, UBX_CLASS_NAV, UBX_NAV_PVT, 1);
    uart_write_bytes(GNSS_UART_NUM, (const char *)cfg_msg_cmd, msg_len);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Включаем NAV-DOP (0x01/0x04) — коэффициенты точности (hDOP, vDOP, pDOP и др.)
    msg_len = ubx_build_cfg_msg(cfg_msg_cmd, UBX_CLASS_NAV, UBX_NAV_DOP, 1);
    uart_write_bytes(GNSS_UART_NUM, (const char *)cfg_msg_cmd, msg_len);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 4. Конфигурация параметров навигации (маски PDOP/PACC, если заданы в конфиге)
    if (g_app_config.pdop_mask > 0 || g_app_config.pacc_mask > 0) {
        uint8_t cfg_nav5_cmd[48];
        size_t  nav5_len = ubx_build_cfg_nav5(cfg_nav5_cmd, g_app_config.pdop_mask, g_app_config.pacc_mask);
        uart_write_bytes(GNSS_UART_NUM, (const char *)cfg_nav5_cmd, nav5_len);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 5. Установка частоты навигационных решений: 1 Гц (1000 мс)
    uint8_t cfg_rate_cmd[16];
    size_t  rate_len = ubx_build_cfg_rate(cfg_rate_cmd, 1000);
    uart_write_bytes(GNSS_UART_NUM, (const char *)cfg_rate_cmd, rate_len);
    vTaskDelay(pdMS_TO_TICKS(50));

    // 6. Сброс парсера перед запуском задачи приёма — исключаем «грязное» состояние FSM
    ubx_parser_init(&s_ubx_parser);

    // 7. Запуск задачи постоянного приёма данных GNSS на ядре 1
    xTaskCreatePinnedToCore(app_gnss_rx_task, "gnss_rx_task", 4096, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "GNSS initialization complete. Waiting for fix...");
    return ESP_OK;
}

bool app_gnss_get_latest_pvt(ubx_nav_pvt_t *pvt)
{
    if (pvt == NULL) {
        return false;
    }
    /* R-4: захватываем мьютекс ДО проверки s_has_pvt, исключая data race на ESP32 LX6 */
    xSemaphoreTake(s_pvt_mutex, portMAX_DELAY);
    if (!s_has_pvt) {
        xSemaphoreGive(s_pvt_mutex);
        return false;
    }
    memcpy(pvt, &s_latest_pvt, sizeof(ubx_nav_pvt_t));
    xSemaphoreGive(s_pvt_mutex);
    return true;
}

bool app_gnss_get_latest_dop(ubx_nav_dop_t *dop)
{
    if (dop == NULL || !s_has_dop) {
        return false;
    }
    xSemaphoreTake(s_pvt_mutex, portMAX_DELAY);
    memcpy(dop, &s_latest_dop, sizeof(ubx_nav_dop_t));
    xSemaphoreGive(s_pvt_mutex);
    return true;
}

bool app_gnss_is_fix_valid(void)
{
    /* R-4: захватываем мьютекс ДО проверки s_has_pvt */
    xSemaphoreTake(s_pvt_mutex, portMAX_DELAY);
    if (!s_has_pvt) {
        xSemaphoreGive(s_pvt_mutex);
        return false;
    }
    bool valid = (s_latest_pvt.fixType == 3 && (s_latest_pvt.flags & 0x01));
    xSemaphoreGive(s_pvt_mutex);
    return valid;
}
