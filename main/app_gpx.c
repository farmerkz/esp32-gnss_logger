/**
 * @file app_gpx.c
 * @brief Реализация инкрементального сохранения GPX трека и восстановления
 *        незакрытых файлов после отключения питания.
 *
 * Изменения:
 *  P-1: добавлен fsync() после fflush() для гарантированной записи на карту.
 *  P-2: алгоритм ремонта переработан — обратный поиск по символу \n с проверкой
 *       формата каждой строки (</trkpt>); количество итераций ограничено.
 *  P-3: после ftruncate() используется fseek(SEEK_SET, valid_size), а не SEEK_END.
 *  R-2: guard-флаг предотвращает двойной запуск задачи GPX.
 *  R-3/G-1: app_buzzer_play() вызывается ПОСЛЕ освобождения g_sd_mutex.
 */

#include "app_gpx.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_sdcard.h"
#include "app_gnss.h"
#include "app_config.h"
#include "app_buzzer.h"
#include "esp_system.h"

static const char *TAG = "GPX";

static FILE *s_current_gpx_file = NULL;
static char s_current_gpx_path[256] = {0};

/* Минимальная длина строки <trkpt ...>...</trkpt>\n в байтах (приблизительно) */
#define GPX_MIN_LINE_LEN 50

/**
 * @brief Восстанавливает структуру незакрытого GPX трека.
 *        Алгоритм: обратный поиск по символу \n с проверкой формата каждой строки.
 *        Должна вызываться только когда мьютекс g_sd_mutex уже захвачен.
 * @param filepath Полный путь к проверяемому файлу.
 * @return esp_err_t Статус выполнения.
 */
esp_err_t app_gpx_repair_and_close(const char *filepath)
{
    if (filepath == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(filepath, "r+b");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open GPX file for repair: %s", filepath);
        return ESP_ERR_NOT_FOUND;
    }

    /* 1. Получаем размер файла */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        unlink(filepath);
        return ESP_OK;
    }

    /* 2. Быстрая проверка: не был ли файл уже корректно закрыт тегом </gpx> */
    {
        long check_len = (size < 32) ? size : 32;
        char tail[33] = {0};
        fseek(f, size - check_len, SEEK_SET);
        fread(tail, 1, check_len, f);
        if (strstr(tail, "</gpx>") != NULL) {
            fclose(f);
            ESP_LOGI(TAG, "GPX file '%s' is already properly closed", filepath);
            return ESP_OK;
        }
    }

    int fd = fileno(f);

    /* 3. Обратный поиск по символу \n с проверкой формата строки.
     *    Ищем последнюю строку, заканчивающуюся тегом </trkpt>.
     *    Ограничение: не более (size / GPX_MIN_LINE_LEN + 2) итераций. */
    long max_iter   = size / GPX_MIN_LINE_LEN + 2;
    long search_pos = size; /* текущий конец диапазона поиска */
    long valid_end  = -1;   /* позиция конца (включительно) последней валидной строки */
    char line_buf[512];

    for (long iter = 0; iter < max_iter && search_pos > 0; iter++) {
        /* Ищем символ \n, двигаясь побайтно от search_pos к началу */
        long nl_pos  = -1;
        long scan    = search_pos - 1;
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
            /* Не нашли \n — начало файла */
            break;
        }

        /* Ищем предыдущий \n, чтобы определить начало строки */
        long prev_nl = -1;
        for (long s2 = nl_pos - 1; s2 >= 0; s2--) {
            fseek(f, s2, SEEK_SET);
            if (fgetc(f) == '\n') {
                prev_nl = s2;
                break;
            }
        }

        long line_start = prev_nl + 1;
        long line_len   = nl_pos - line_start; /* длина строки без \n */

        if (line_len > 0 && line_len < (long)(sizeof(line_buf) - 1)) {
            fseek(f, line_start, SEEK_SET);
            size_t rd = fread(line_buf, 1, (size_t)line_len, f);
            line_buf[rd] = '\0';

            /* Проверяем: строка должна заканчиваться тегом </trkpt> */
            const char *end_tag = "</trkpt>";
            size_t tag_len = strlen(end_tag);
            if (rd >= tag_len && memcmp(line_buf + rd - tag_len, end_tag, tag_len) == 0) {
                /* Нашли корректную последнюю точку — усекаем файл до nl_pos+1 */
                valid_end = nl_pos + 1;
                break;
            }
        }

        /* Строка некорректна — сдвигаем позицию поиска выше */
        search_pos = nl_pos;
    }

    /* 4. Определяем позицию усечения */
    long trunc_pos;
    if (valid_end > 0) {
        trunc_pos = valid_end;
        ESP_LOGW(TAG, "Truncating GPX '%s' to last valid </trkpt> at byte %ld (was %ld)",
                 filepath, trunc_pos, size);
    } else {
        /* Корректных точек нет — усекаем до тега <trkseg> (сохраняем заголовок) */
        long head_read = (size > 1024) ? 1024 : size;
        char *head_buf = malloc(head_read + 1);
        trunc_pos = 0;
        if (head_buf) {
            fseek(f, 0, SEEK_SET);
            size_t hr = fread(head_buf, 1, (size_t)head_read, f);
            head_buf[hr] = '\0';
            const char *seg_tag = "<trkseg>";
            char *seg_ptr = strstr(head_buf, seg_tag);
            if (seg_ptr) {
                trunc_pos = (long)(seg_ptr - head_buf) + (long)strlen(seg_tag);
            }
            free(head_buf);
        }
        ESP_LOGW(TAG, "No valid trkpt found in GPX '%s'. Keeping header up to byte %ld",
                 filepath, trunc_pos);
    }

    /* 5. Усекаем файл.
     *    P-3: после ftruncate(fd) синхронизируем позицию FILE через fseek(SEEK_SET),
     *    а не через fseek(SEEK_END), так как FILE не знает об усечении. */
    if (trunc_pos > 0 && trunc_pos < size) {
        fflush(f);
        if (fd >= 0) {
            ftruncate(fd, trunc_pos);
        }
        fflush(f);
        fseek(f, trunc_pos, SEEK_SET); /* P-3: SEEK_SET, не SEEK_END */
    } else if (trunc_pos == 0) {
        fseek(f, 0, SEEK_END);
    } else {
        fseek(f, 0, SEEK_END);
    }

    /* 6. Дописываем корректный эпилог GPX */
    fprintf(f, "\n</trkseg>\n</trk>\n</gpx>\n");

    /* P-1: fsync для гарантированной записи на физическую карту */
    fflush(f);
    if (fd >= 0) {
        fsync(fd);
    }
    fclose(f);

    ESP_LOGI(TAG, "GPX file successfully repaired and closed: %s", filepath);
    return ESP_OK;
}

static void app_gpx_task(void *pvParameters)
{
    ubx_nav_pvt_t pvt;
    char date_str[32];
    char time_str[32];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* Запись раз в секунду при валидном фиксе */

        if (app_gnss_get_latest_pvt(&pvt) && app_gnss_is_fix_valid()) {
            snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", pvt.year, pvt.month, pvt.day);
            snprintf(time_str, sizeof(time_str), "%02d%02d%02d", pvt.hour, pvt.min, pvt.sec);

            /* Флаг: воспроизвести FILE_ROTATE ПОСЛЕ освобождения мьютекса (R-3/G-1) */
            bool play_rotate = false;

            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);

            /* Если файл не открыт — создаём новый трек в папке gps.wk */
            if (s_current_gpx_file == NULL) {
                snprintf(s_current_gpx_path, sizeof(s_current_gpx_path),
                         "%s/gps-%s_%s.gpx", DIR_GPS_WK, date_str, time_str);

                s_current_gpx_file = fopen(s_current_gpx_path, "w");
                if (s_current_gpx_file != NULL) {
                    fprintf(s_current_gpx_file,
                            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                            "<gpx version=\"1.0\" creator=\"ESP32 GNSS Logger\" xmlns=\"http://www.topografix.com/GPX/1/0\">\n"
                            "<name>GPS-%s</name>\n"
                            "<trk><name>TRK-%sT%02d:%02d:%02d-%s</name>\n<trkseg>\n",
                            date_str, date_str, pvt.hour, pvt.min, pvt.sec, g_app_config.module_id);
                    /* P-1: fsync после заголовка */
                    fflush(s_current_gpx_file);
                    fsync(fileno(s_current_gpx_file));
                    ESP_LOGI(TAG, "Created new GPX file: %s", s_current_gpx_path);
                    /* R-3/G-1: звук после освобождения мьютекса */
                    play_rotate = true;
                }
            }

            if (s_current_gpx_file != NULL) {
                double lat    = pvt.lat     * 1e-7;
                double lon    = pvt.lon     * 1e-7;
                double height = pvt.height  * 1e-3;
                double speed  = pvt.gSpeed  * 1e-3;
                double course = pvt.headMot * 1e-5;
                double pdop   = pvt.pDOP    * 0.01;
                double hmsl   = pvt.hMSL    * 1e-3;
                double hacc   = pvt.hAcc    * 1e-3;
                double vacc   = pvt.vAcc    * 1e-3;

                fprintf(s_current_gpx_file,
                        "<trkpt lat=\"%.7f\" lon=\"%.7f\">"
                        "<time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>"
                        "<ele>%.2f</ele>"
                        "<speed>%.2f</speed>"
                        "<course>%.2f</course>"
                        "<sat>%d</sat>"
                        "<pdop>%.2f</pdop>"
                        "<hmsl>%.2f</hmsl>"
                        "<fix>3d</fix>"
                        "<hacc>%.2f</hacc>"
                        "<vacc>%.2f</vacc>"
                        "<extensions><heap>%lu</heap></extensions>"
                        "</trkpt>\n",
                        lat, lon,
                        pvt.year, pvt.month, pvt.day, pvt.hour, pvt.min, pvt.sec,
                        height, speed, course, pvt.numSV, pdop, hmsl, hacc, vacc,
                        (unsigned long)esp_get_free_internal_heap_size());

                /* P-1: fflush + fsync — принудительная запись на физическую карту */
                fflush(s_current_gpx_file);
                fsync(fileno(s_current_gpx_file));
                ESP_LOGD(TAG, "Written GPX point at lat=%.7f, lon=%.7f", lat, lon);

                /* Ротация при превышении 50 МБ */
                fseek(s_current_gpx_file, 0, SEEK_END);
                long current_size = ftell(s_current_gpx_file);
                if (current_size >= 52428800) {
                    fclose(s_current_gpx_file);
                    s_current_gpx_file = NULL;

                    app_gpx_repair_and_close(s_current_gpx_path);

                    char ready_path[256];
                    snprintf(ready_path, sizeof(ready_path), "%s/gps-%s_%s.gpx",
                             DIR_GPS_RD, date_str, time_str);
                    rename(s_current_gpx_path, ready_path);
                    ESP_LOGI(TAG, "GPX file reached 50MB. Rotated to ready: %s", ready_path);
                    play_rotate = true; /* R-3 */
                }
            }

            xSemaphoreGive(g_sd_mutex);

            /* R-3/G-1: воспроизводим звук ПОСЛЕ освобождения g_sd_mutex */
            if (play_rotate) {
                app_buzzer_play(APP_BUZZER_FILE_ROTATE);
            }
        }
    }
}

esp_err_t app_gpx_init(void)
{
    /* R-2: guard-флаг предотвращает двойной запуск задачи */
    static bool s_initialized = false;
    if (s_initialized) {
        ESP_LOGW(TAG, "app_gpx_init() called more than once — ignored");
        return ESP_OK;
    }
    s_initialized = true;

    xTaskCreatePinnedToCore(app_gpx_task, "gpx_task", 4096, NULL, 4, NULL, 1);
    return ESP_OK;
}
