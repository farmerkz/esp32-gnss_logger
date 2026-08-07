/**
 * @file app_wigle.c
 * @brief Реализация фонового сканирования WiFi и форматирования Wigle CSV v1.4.
 *
 * Изменения:
 *  W-1: реализована app_wigle_repair_csv() — восстановление последней строки CSV
 *       путём обратного поиска по символу \n с проверкой формата (11 полей).
 *  R-2: guard-флаг предотвращает двойной запуск задачи.
 *  R-3: app_buzzer_play() вызывается ПОСЛЕ освобождения g_sd_mutex.
 *  R-1: задача не запускается при активном веб-сервере (управляется из main.c).
 */

#include "app_wigle.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_sdcard.h"
#include "app_gnss.h"
#include "app_config.h"
#include "app_buzzer.h"
#include "app_wifi.h"

static const char *TAG = "WIGLE";

static FILE *s_current_wifi_file = NULL;
static char s_current_wifi_path[256] = {0};

/* Минимальная длина корректной строки данных CSV (приблизительно) */
#define CSV_MIN_LINE_LEN 40
/* Ожидаемое количество полей в строке данных Wigle CSV v1.4 */
#define CSV_FIELD_COUNT 11

static const char *app_wigle_auth_to_str(wifi_auth_mode_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "[Open]";
    case WIFI_AUTH_WEP:             return "[WEP][ESS]";
    case WIFI_AUTH_WPA_PSK:         return "[WPA-PSK-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA2_PSK:        return "[WPA2-PSK-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-ENTERPRISE-CCMP+TKIP][ESS]";
    case WIFI_AUTH_WPA3_PSK:        return "[WPA3-PSK][ESS]";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2+WPA3-PSK][ESS]";
    default:                        return "[UNKNOWN]";
    }
}

/**
 * @brief Подсчитывает количество запятых в строке (не вложенных).
 *        Используется для проверки формата CSV строки.
 */
static int count_commas(const char *line, size_t len)
{
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        if (line[i] == ',') count++;
    }
    return count;
}

/**
 * @brief Проверяет и восстанавливает последнюю строку Wigle CSV файла.
 *        Алгоритм: обратный поиск по символу \n, проверка 11 полей CSV.
 *        Должна вызываться под мьютексом g_sd_mutex.
 */
esp_err_t app_wigle_repair_csv(const char *filepath)
{
    if (filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(filepath, "r+b");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open CSV for repair: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return ESP_OK;
    }

    int fd = fileno(f);

    /* Ограничение итераций: не более size / CSV_MIN_LINE_LEN + 2 */
    long max_iter   = size / CSV_MIN_LINE_LEN + 2;
    long search_pos = size;
    char line_buf[512];

    for (long iter = 0; iter < max_iter && search_pos > 0; iter++) {
        /* Ищем \n с конца текущей позиции */
        long nl_pos = -1;
        long scan   = search_pos - 1;
        while (scan >= 0) {
            fseek(f, scan, SEEK_SET);
            int c = fgetc(f);
            if (c == '\n') {
                nl_pos = scan;
                break;
            }
            scan--;
        }

        if (nl_pos < 0) {
            /* Не нашли \n — файл состоит из одной строки без \n */
            break;
        }

        /* Усекаем файл до nl_pos (включительно, т.е. до nl_pos+1 символов) */
        fflush(f);
        if (fd >= 0) {
            ftruncate(fd, nl_pos + 1);
        }
        fflush(f);
        fseek(f, nl_pos + 1, SEEK_SET);

        /* Ищем предыдущий \n, чтобы вычитать строку перед найденным \n */
        long prev_nl = -1;
        for (long s2 = nl_pos - 1; s2 >= 0; s2--) {
            fseek(f, s2, SEEK_SET);
            if (fgetc(f) == '\n') {
                prev_nl = s2;
                break;
            }
        }

        long line_start = prev_nl + 1;
        long line_len   = nl_pos - line_start;

        if (line_len <= 0) {
            /* Пустая строка — продолжаем поиск выше */
            search_pos = nl_pos;
            continue;
        }

        if (line_len < (long)(sizeof(line_buf) - 1)) {
            fseek(f, line_start, SEEK_SET);
            size_t rd = fread(line_buf, 1, (size_t)line_len, f);
            line_buf[rd] = '\0';

            /* Пропускаем строки заголовков (начинаются с "WigleWifi" или "MAC,") */
            if (strncmp(line_buf, "WigleWifi", 9) == 0 ||
                strncmp(line_buf, "MAC,", 4) == 0) {
                /* Файл содержит только заголовки — это нормально */
                ESP_LOGI(TAG, "CSV '%s' contains only header lines, OK", filepath);
                fclose(f);
                return ESP_OK;
            }

            /* Проверяем: ровно (CSV_FIELD_COUNT - 1) запятых = CSV_FIELD_COUNT полей */
            int commas = count_commas(line_buf, rd);
            if (commas == CSV_FIELD_COUNT - 1) {
                /* Строка корректна */
                ESP_LOGI(TAG, "CSV '%s' last line is valid (%d fields)", filepath, CSV_FIELD_COUNT);
                fclose(f);
                return ESP_OK;
            }

            ESP_LOGD(TAG, "CSV line has %d commas (expected %d), retrying...",
                     commas, CSV_FIELD_COUNT - 1);
        }

        /* Строка некорректна — продолжаем поиск выше */
        search_pos = nl_pos;
    }

    fclose(f);
    ESP_LOGW(TAG, "CSV '%s': no valid data line found after repair attempts", filepath);
    return ESP_FAIL;
}

typedef enum {
    SCAN_ACTIVE,
    SCAN_PAUSED,
    SCAN_BACKOFF
} scan_state_t;

static void app_wigle_task(void *pvParameters)
{
    ubx_nav_pvt_t pvt;
    scan_state_t state = SCAN_ACTIVE;
    int ap_found_count = 0;
    int ap_lost_count = 0;
    const int THRESHOLD = 3;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000)); /* Сканирование раз в 5 секунд */

        EventBits_t bits = xEventGroupGetBits(g_network_event_group);
        bool wifi_locked = (bits & BIT_WIFI_FAILED) != 0;

        if (state == SCAN_PAUSED) {
            if (wifi_locked) {
                ESP_LOGW(TAG, "WiFi locked out. Scanner entering BACKOFF state.");
                state = SCAN_BACKOFF;
                ap_lost_count = 0;
            }
            if (state == SCAN_PAUSED) {
                continue; // Полная пауза сканирования
            }
        }

        /* Запуск сканирования эфира */
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time = {
                .active = { .min = 100, .max = 300 }
            }
        };

        if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
            continue;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        bool target_ap_found = false;

        wifi_ap_record_t *ap_records = NULL;
        if (ap_count > 0) {
            ap_records = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (ap_records == NULL) {
                continue;
            }

            if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) != ESP_OK) {
                free(ap_records);
                continue;
            }

            /* Ищем целевую точку доступа */
            for (uint16_t i = 0; i < ap_count; i++) {
                if (strcmp((char *)ap_records[i].ssid, g_app_config.wifi_ssid) == 0) {
                    target_ap_found = true;
                    break;
                }
            }

            /* Запись на SD только в состоянии SCAN_ACTIVE */
            if (state == SCAN_ACTIVE) {
                if (app_gnss_get_latest_pvt(&pvt) && app_gnss_is_fix_valid()) {
                    char date_str[32];
                    char time_str[32];
                    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", pvt.year, pvt.month, pvt.day);
                    snprintf(time_str, sizeof(time_str), "%02d%02d%02d", pvt.hour, pvt.min, pvt.sec);

                    bool play_rotate = false;
                    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);

                    if (s_current_wifi_file == NULL) {
                        snprintf(s_current_wifi_path, sizeof(s_current_wifi_path),
                                 "%s/wifi-%s_%s.csv", DIR_WIFI_WK, date_str, time_str);

                        s_current_wifi_file = fopen(s_current_wifi_path, "w");
                        if (s_current_wifi_file != NULL) {
                            fprintf(s_current_wifi_file,
                                    "WigleWifi-1.4,appRelease=2.26,model=HomeModel,release=0.0.2,"
                                    "device=esp32Wardriving,display=none,board=esp32-based,brand=Espressif\n"
                                    "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,"
                                    "CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n");
                            fflush(s_current_wifi_file);
                            ESP_LOGI(TAG, "Created new Wigle CSV file: %s", s_current_wifi_path);
                        }
                    }

                    if (s_current_wifi_file != NULL) {
                        double lat = pvt.lat    * 1e-7;
                        double lon = pvt.lon    * 1e-7;
                        double alt = pvt.height * 1e-3;
                        double acc = pvt.hAcc   * 1e-3;

                        for (uint16_t i = 0; i < ap_count; i++) {
                            char bssid_str[20];
                            snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                                     ap_records[i].bssid[0], ap_records[i].bssid[1], ap_records[i].bssid[2],
                                     ap_records[i].bssid[3], ap_records[i].bssid[4], ap_records[i].bssid[5]);

                            fprintf(s_current_wifi_file,
                                    "%s,%s,%s,%s %02d:%02d:%02d,%d,%d,%.8f,%.8f,%d,%.8f,WIFI\n",
                                    bssid_str,
                                    (char *)ap_records[i].ssid,
                                    app_wigle_auth_to_str(ap_records[i].authmode),
                                    date_str, pvt.hour, pvt.min, pvt.sec,
                                    ap_records[i].primary,
                                    ap_records[i].rssi,
                                    lat, lon, (int)alt, acc);
                        }
                        fflush(s_current_wifi_file);

                        fseek(s_current_wifi_file, 0, SEEK_END);
                        long current_size = ftell(s_current_wifi_file);
                        if (current_size >= (long)g_app_config.filesize_limit) {
                            fclose(s_current_wifi_file);
                            s_current_wifi_file = NULL;

                            char ready_path[256];
                            snprintf(ready_path, sizeof(ready_path), "%s/wifi-%s_%s.csv",
                                     DIR_WIFI_RD, date_str, time_str);
                            rename(s_current_wifi_path, ready_path);
                            ESP_LOGI(TAG, "Wigle CSV reached size limit. Rotated.");
                            play_rotate = true;
                        }
                    }
                    xSemaphoreGive(g_sd_mutex);

                    if (play_rotate) {
                        app_buzzer_play(APP_BUZZER_FILE_ROTATE);
                    }
                }
            }
            free(ap_records);
        }

        /* Обработка переходов конечного автомата (FSM) */
        if (state == SCAN_ACTIVE) {
            if (target_ap_found) {
                ap_found_count++;
                if (ap_found_count >= THRESHOLD) {
                    ESP_LOGI(TAG, "Target AP stable. Triggering WiFi connection.");
                    xEventGroupSetBits(g_network_event_group, BIT_AP_AVAILABLE);
                    state = SCAN_PAUSED;
                }
            } else {
                ap_found_count = 0;
            }
        } else if (state == SCAN_BACKOFF) {
            if (!target_ap_found) {
                ap_lost_count++;
                if (ap_lost_count >= THRESHOLD) {
                    ESP_LOGI(TAG, "Target AP successfully cleared. Resuming Wardriving.");
                    xEventGroupClearBits(g_network_event_group, BIT_AP_AVAILABLE);
                    state = SCAN_ACTIVE;
                    ap_found_count = 0;
                }
            } else {
                ap_lost_count = 0;
            }
        }
    }
}

esp_err_t app_wigle_init(void)
{
    /* R-2: guard-флаг предотвращает двойной запуск задачи */
    static bool s_initialized = false;
    if (s_initialized) {
        ESP_LOGW(TAG, "app_wigle_init() called more than once — ignored");
        return ESP_OK;
    }
    s_initialized = true;

    xTaskCreatePinnedToCore(app_wigle_task, "wigle_task", 4096, NULL, 3, NULL, 1);
    return ESP_OK;
}
