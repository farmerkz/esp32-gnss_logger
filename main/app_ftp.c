/**
 * @file app_ftp.c
 * @brief Легковесная реализация FTP клиента на чистых BSD сокетах с обработкой ошибок.
 *
 * Логика управления подключением к Wi-Fi (приоритеты):
 *
 *  ПРИОРИТЕТ 1 — Веб-сервер (webserver_enable=true + логин/пароль заданы):
 *    Подключение к AP поддерживается постоянно. При потере связи выполняются
 *    попытки переподключения с экспоненциальным backoff (1-3 попытки: 60 сек,
 *    далее: 300 сек). Wi-Fi НЕ отключается пока выполняются условия веб-сервера.
 *
 *  ПРИОРИТЕТ 2 — FTP отгрузка файлов (gps_send или wifi_send):
 *    Каждые 30 секунд проверяется наличие файлов в папках .rd.
 *    Если файлы есть:
 *      - Веб-сервер активен: используется уже установленное соединение Wi-Fi.
 *        Принудительное переподключение не выполняется — управляет логика прио. 1.
 *        Если AP недоступен (backoff) — FTP пропускается в этой итерации.
 *      - Веб-сервер выключен: выполняется разовое подключение к AP для FTP.
 *        После завершения отгрузки AP отключается.
 *
 *  Активный веб-сервер не блокирует и не мешает FTP отгрузке файлов.
 *
 * Совместимость: ProFTPD (поддерживает многострочные ответы 220-, PASV, RNFR/RNTO).
 */

#include "app_ftp.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app_config.h"
#include "app_sdcard.h"
#include "app_wifi.h"
#include "app_buzzer.h"
#include "app_webserver.h"

static const char *TAG = "FTP";

/* =========================================================================
 * Вспомогательные функции FTP протокола
 * ========================================================================= */

/**
 * @brief Чтение одного полного FTP-ответа из сокета.
 *
 * Протокол FTP допускает многострочные ответы (RFC 959):
 *   - Промежуточные строки: "NNN-текст\r\n"
 *   - Финальная строка:     "NNN текст\r\n"  (пробел после кода вместо '-')
 *
 * ProFTPD использует многострочное приветствие 220-, поэтому необходимо
 * читать в цикле до получения финальной строки "NNN ".
 *
 * @param sock    Дескриптор управляющего сокета.
 * @param buf     Буфер для ответа.
 * @param buf_len Размер буфера.
 * @return Числовой код FTP-ответа (например 220, 331, 230) или -1 при ошибке.
 */
static int ftp_read_response(int sock, char *buf, size_t buf_len)
{
    memset(buf, 0, buf_len);
    size_t total = 0;
    int    code  = -1;

    while (total < buf_len - 1) {
        int n = read(sock, buf + total, buf_len - 1 - total);
        if (n <= 0) {
            /* Таймаут или разрыв соединения */
            ESP_LOGE(TAG, "ftp_read_response: read error n=%d errno=%d", n, errno);
            return -1;
        }
        total += n;
        buf[total] = '\0';

        /*
         * Анализируем накопленный буфер построчно.
         * Ищем финальную строку ответа вида "NNN " (код + пробел).
         * Промежуточные строки имеют вид "NNN-" (код + дефис) — пропускаем.
         */
        char *line = buf;
        int   last_code = -1;
        bool  found_final = false;

        while (line < buf + total) {
            /* Ищем конец текущей строки */
            char *eol = memchr(line, '\n', (buf + total) - line);
            if (!eol) {
                /* Строка ещё не полная — ждём следующего read */
                break;
            }

            /* Парсим трёхзначный код ответа */
            if ((eol - line) >= 4) {
                char code_str[4] = { line[0], line[1], line[2], '\0' };
                int parsed = atoi(code_str);
                if (parsed >= 100 && parsed <= 999) {
                    last_code = parsed;
                    /* Финальная строка: "NNN " (пробел на 4-й позиции) */
                    if (line[3] == ' ') {
                        found_final = true;
                    }
                }
            }
            line = eol + 1;
        }

        if (found_final) {
            code = last_code;
            break;
        }
    }

    return code;
}

/**
 * @brief Отправка FTP-команды и чтение ответа сервера.
 * @param sock    Дескриптор управляющего сокета.
 * @param cmd     Строка команды (включая \r\n).
 * @param buf     Буфер для ответа.
 * @param buf_len Размер буфера.
 * @return Числовой код FTP-ответа или -1 при ошибке I/O.
 */
static int send_cmd(int sock, const char *cmd, char *buf, size_t buf_len)
{
    if (write(sock, cmd, strlen(cmd)) < 0) {
        ESP_LOGE(TAG, "send_cmd write error: errno=%d", errno);
        return -1;
    }
    return ftp_read_response(sock, buf, buf_len);
}

/**
 * @brief Парсинг адреса и порта из ответа PASV (формат RFC 959).
 * @param resp     Строка ответа 227 от сервера.
 * @param ip_out   Буфер для IPv4 адреса (минимум 16 байт).
 * @param port_out Указатель для записи номера порта.
 * @return true при успешном разборе.
 */
static bool parse_pasv_response(const char *resp, char *ip_out, int *port_out)
{
    const char *start = strchr(resp, '(');
    if (!start) return false;
    start++;
    int h1, h2, h3, h4, p1, p2;
    if (sscanf(start, "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) == 6) {
        snprintf(ip_out, 16, "%d.%d.%d.%d", h1, h2, h3, h4);
        *port_out = p1 * 256 + p2;
        return true;
    }
    return false;
}

/**
 * @brief Отправка одного файла на FTP сервер с атомарной загрузкой через .partial.
 *
 * Алгоритм:
 *  1. TCP-соединение с управляющим сокетом → приветствие 220.
 *  2. Аутентификация USER/PASS.
 *  3. TYPE I (бинарный режим).
 *  4. PASV → разбор адреса/порта → TCP-соединение с сокетом данных.
 *  5. STOR <имя>.partial → ожидание 125/150 → передача данных файла.
 *  6. Закрытие сокета данных → ожидание 226 Transfer complete.
 *  7. RNFR / RNTO — переименование из .partial в финальное имя.
 *  8. QUIT.
 *
 * fopen выполняется под мьютексом g_sd_mutex; fread — без (файл в .rd только читается).
 *
 * @param local_path      Полный путь к файлу на SD-карте.
 * @param remote_filename Имя файла на FTP сервере (без пути).
 * @return true при успешной отправке.
 */
static bool app_ftp_send_file(const char *local_path, const char *remote_filename)
{
    /* Буфер для FTP-ответов: 1024 байт достаточно для всех ответов ProFTPD,
     * включая многострочное приветствие 220-. */
    char buf[1024];
    char cmd[1024];
    int  code;

    /* ----------------------------------------------------------------
     * Шаг 1: Создаём и подключаем управляющий сокет
     * ---------------------------------------------------------------- */
    int ctrl_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctrl_sock < 0) {
        ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
        return false;
    }

    /* Таймаут 10 сек — достаточно для медленных сетей */
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ctrl_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(g_app_config.ftp_port);
    inet_pton(AF_INET, g_app_config.ftp_address, &server_addr.sin_addr);

    if (connect(ctrl_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "connect() to %s:%d failed: errno=%d",
                 g_app_config.ftp_address, g_app_config.ftp_port, errno);
        close(ctrl_sock);
        return false;
    }

    /* ----------------------------------------------------------------
     * Шаг 2: Читаем приветствие сервера (220 / 220-)
     * ProFTPD отправляет многострочный баннер, завершающийся "220 ready"
     * ---------------------------------------------------------------- */
    code = ftp_read_response(ctrl_sock, buf, sizeof(buf));
    if (code != 220) {
        ESP_LOGE(TAG, "FTP greeting failed, code=%d, resp='%.120s'", code, buf);
        close(ctrl_sock);
        return false;
    }
    ESP_LOGD(TAG, "FTP server greeting: %.80s", buf);

    /* ----------------------------------------------------------------
     * Шаг 3: Аутентификация USER / PASS
     * ---------------------------------------------------------------- */
    snprintf(cmd, sizeof(cmd), "USER %s\r\n", g_app_config.ftp_user);
    code = send_cmd(ctrl_sock, cmd, buf, sizeof(buf));
    if (code != 331) {
        ESP_LOGE(TAG, "USER '%s' rejected, code=%d, resp='%.80s'",
                 g_app_config.ftp_user, code, buf);
        close(ctrl_sock);
        return false;
    }

    snprintf(cmd, sizeof(cmd), "PASS %s\r\n", g_app_config.ftp_passwd);
    code = send_cmd(ctrl_sock, cmd, buf, sizeof(buf));
    if (code != 230) {
        ESP_LOGE(TAG, "PASS rejected (wrong credentials?), code=%d, resp='%.80s'",
                 code, buf);
        close(ctrl_sock);
        return false;
    }

    /* ----------------------------------------------------------------
     * Шаг 4: Бинарный режим TYPE I
     * ---------------------------------------------------------------- */
    code = send_cmd(ctrl_sock, "TYPE I\r\n", buf, sizeof(buf));
    if (code != 200) {
        ESP_LOGW(TAG, "TYPE I: unexpected code=%d (продолжаем)", code);
    }

    /* ----------------------------------------------------------------
     * Шаг 5: Пассивный режим PASV → подключение к сокету данных
     * ---------------------------------------------------------------- */
    code = send_cmd(ctrl_sock, "PASV\r\n", buf, sizeof(buf));
    if (code != 227) {
        ESP_LOGE(TAG, "PASV failed, code=%d, resp='%.80s'", code, buf);
        close(ctrl_sock);
        return false;
    }

    char data_ip[16];
    int  data_port = 0;
    if (!parse_pasv_response(buf, data_ip, &data_port)) {
        ESP_LOGE(TAG, "PASV parse failed, resp='%.80s'", buf);
        close(ctrl_sock);
        return false;
    }
    ESP_LOGD(TAG, "PASV data channel: %s:%d", data_ip, data_port);

    int data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (data_sock < 0) {
        ESP_LOGE(TAG, "data socket() failed: errno=%d", errno);
        close(ctrl_sock);
        return false;
    }
    setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(data_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in data_addr;
    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_port   = htons(data_port);
    inet_pton(AF_INET, data_ip, &data_addr.sin_addr);

    if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) < 0) {
        ESP_LOGE(TAG, "data connect() to %s:%d failed: errno=%d",
                 data_ip, data_port, errno);
        close(data_sock);
        close(ctrl_sock);
        return false;
    }

    /* ----------------------------------------------------------------
     * Шаг 6: STOR — команда загрузки во временный .partial файл
     * По протоколу FTP необходимо дождаться ответа 125 или 150 прежде
     * чем начать передачу данных.
     * ---------------------------------------------------------------- */
    const char *ext = strrchr(remote_filename, '.');
    char name_without_ext[300] = {0};
    if (ext) {
        size_t name_len = ext - remote_filename;
        strncpy(name_without_ext, remote_filename, name_len);
    } else {
        strcpy(name_without_ext, remote_filename);
        ext = "";
    }

    char partial_name[512];
    snprintf(partial_name, sizeof(partial_name), "%s-%s%s.partial",
             name_without_ext, g_app_config.module_id, ext);

    snprintf(cmd, sizeof(cmd), "STOR %s\r\n", partial_name);
    code = send_cmd(ctrl_sock, cmd, buf, sizeof(buf));
    if (code != 125 && code != 150) {
        ESP_LOGE(TAG, "STOR '%s' rejected, code=%d, resp='%.80s'",
                 partial_name, code, buf);
        close(data_sock);
        close(ctrl_sock);
        return false;
    }
    ESP_LOGD(TAG, "STOR '%s' accepted (code=%d), sending data...", partial_name, code);

    /* ----------------------------------------------------------------
     * Шаг 7: Открываем файл под мьютексом и передаём данные
     * В соответствии с 3.5, fopen, fread и fclose выполняются под мьютексом.
     * ---------------------------------------------------------------- */
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(local_path, "rb");
    xSemaphoreGive(g_sd_mutex);

    if (f == NULL) {
        ESP_LOGE(TAG, "fopen('%s') failed: errno=%d", local_path, errno);
        close(data_sock);
        close(ctrl_sock);
        return false;
    }

    ESP_LOGI(TAG, "FTP: Opened file '%s' for uploading", local_path);

    char *file_buf = malloc(2048); /* Буфер чтения выделяем в куче для экономии стека */
    if (!file_buf) {
        ESP_LOGE(TAG, "malloc failed for file_buf");
        fclose(f);
        close(data_sock);
        close(ctrl_sock);
        return false;
    }

    size_t r        = 0;
    bool   send_ok  = true;

    /* M-1: файл из .rd читается исключительно FTP задачей —
     * захват g_sd_mutex на каждый fread избыточен и блокирует GPX/syslog.
     * Мьютекс захватывается только при fclose. */
    while (true) {
        r = fread(file_buf, 1, 2048, f);

        if (r == 0) {
            break;
        }

        ssize_t written = write(data_sock, file_buf, r);
        if (written != (ssize_t)r) {
            ESP_LOGE(TAG, "data write error: written=%d expected=%d errno=%d",
                     (int)written, (int)r, errno);
            send_ok = false;
            break;
        }
    }

    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    if (ferror(f)) {
        ESP_LOGE(TAG, "fread error on '%s'", local_path);
        send_ok = false;
    }
    fclose(f);
    xSemaphoreGive(g_sd_mutex);
    free(file_buf);

    ESP_LOGI(TAG, "FTP: Finished sending data for '%s', closed file", local_path);

    close(data_sock); /* Закрытие сокета данных сигнализирует серверу об окончании */

    /* ----------------------------------------------------------------
     * Шаг 8: Ожидаем ответ 226 Transfer complete
     * ---------------------------------------------------------------- */
    code = ftp_read_response(ctrl_sock, buf, sizeof(buf));
    if (code != 226) {
        ESP_LOGW(TAG, "After data transfer: expected 226, got code=%d, resp='%.80s'",
                 code, buf);
        /* Не считаем фатальной ошибкой — данные могли дойти корректно */
    }

    /* ----------------------------------------------------------------
     * Шаг 9: Переименовываем .partial → финальное имя
     * RNFR / RNTO поддерживаются ProFTPD (и большинством FTP серверов).
     * ---------------------------------------------------------------- */
    if (send_ok) {
        char final_name[512];
        snprintf(final_name, sizeof(final_name), "%s-%s%s",
                 name_without_ext, g_app_config.module_id, ext);

        snprintf(cmd, sizeof(cmd), "RNFR %s\r\n", partial_name);
        code = send_cmd(ctrl_sock, cmd, buf, sizeof(buf));
        if (code != 350) {
            ESP_LOGW(TAG, "RNFR '%s' failed, code=%d (файл может остаться как .partial)",
                     partial_name, code);
            send_ok = false; /* Помечаем как неудачу — файл не переименован */
        } else {
            snprintf(cmd, sizeof(cmd), "RNTO %s\r\n", final_name);
            code = send_cmd(ctrl_sock, cmd, buf, sizeof(buf));
            if (code != 250) {
                ESP_LOGW(TAG, "RNTO '%s' failed, code=%d", final_name, code);
                send_ok = false;
            } else {
                ESP_LOGI(TAG, "Remote rename: '%s' -> '%s'", partial_name, final_name);
            }
        }
    }

    /* ----------------------------------------------------------------
     * Шаг 10: Завершение сессии
     * ---------------------------------------------------------------- */
    send_cmd(ctrl_sock, "QUIT\r\n", buf, sizeof(buf));
    close(ctrl_sock);

    return send_ok;
}

/* =========================================================================
 * Вспомогательные функции работы с директориями
 * ========================================================================= */

/**
 * @brief Проверяет наличие файлов в указанной директории.
 *        Полный цикл opendir/readdir/closedir выполняется под мьютексом (§3.5).
 * @param dir_path Путь к директории.
 * @return true если в директории есть хотя бы один файл.
 */
static bool has_files_in_dir(const char *dir_path)
{
    bool found = false;
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type == DT_REG) {
                found = true;
                break;
            }
        }
        closedir(dir);
    }
    xSemaphoreGive(g_sd_mutex);
    return found;
}

/**
 * @brief Проверяет наличие файлов, готовых к FTP-отгрузке (в папках .rd).
 *        Учитывает флаги gps_send / wifi_send из конфигурации.
 * @return true если хотя бы один файл ожидает отправки.
 */
static bool has_files_for_upload(void)
{
    if (g_app_config.gps_send && has_files_in_dir(DIR_GPS_RD)) {
        return true;
    }
    if (g_app_config.wifi_send && has_files_in_dir(DIR_WIFI_RD)) {
        return true;
    }
    return false;
}

/**
 * @brief Последовательная отправка всех файлов из директории src_dir на FTP
 *        с перемещением в dst_dir после успешной отправки.
 *
 * Паттерн "по одному файлу за итерацию":
 *   1. Взять мьютекс → найти имя одного файла → closedir → отдать мьютекс
 *   2. Отправить файл по FTP (без мьютекса — долгая сетевая операция)
 *   3. При успехе: взять мьютекс → rename в dst_dir → отдать мьютекс
 *   4. При ошибке: прерываем — сетевая проблема, следующие файлы тоже не уйдут
 *   5. Повторить с шага 1 до исчерпания файлов или первой сетевой ошибки
 *
 * @param src_dir  Источник (папка .rd).
 * @param dst_dir  Назначение (папка .sd).
 * @return true если был загружен хотя бы один файл.
 */
static bool app_ftp_upload_dir(const char *src_dir, const char *dst_dir)
{
    bool any_uploaded = false;
    char filename[256];
    char src_path[512];
    char dst_path[512];

    while (true) {
        /* Шаг 1: под мьютексом находим имя следующего файла для отправки */
        bool found = false;
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        DIR *dir = opendir(src_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (entry->d_type == DT_REG) {
                    strncpy(filename, entry->d_name, sizeof(filename) - 1);
                    filename[sizeof(filename) - 1] = '\0';
                    found = true;
                    break;
                }
            }
            closedir(dir);
        }
        xSemaphoreGive(g_sd_mutex);

        if (!found) {
            break; /* Все файлы обработаны */
        }

        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, filename);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, filename);

        /* Определяем целевую папку на FTP сервере на основе пути источника */
        char remote_path[300];
        const char *remote_dir = "";
        if (strstr(src_dir, "gps.rd") != NULL) {
            remote_dir = "gps.rd/";
        } else if (strstr(src_dir, "wifi.rd") != NULL) {
            remote_dir = "wifi.rd/";
        }
        snprintf(remote_path, sizeof(remote_path), "%s%s", remote_dir, filename);

        /* Шаг 2: отправка по FTP (без мьютекса — долгая сетевая операция) */
        if (app_ftp_send_file(src_path, remote_path)) {
            /* Шаг 3: под мьютексом перемещаем файл в папку .sd */
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            rename(src_path, dst_path);
            xSemaphoreGive(g_sd_mutex);
            ESP_LOGI(TAG, "Uploaded & moved: %s", filename);
            any_uploaded = true;
        } else {
            /* Ошибка отправки: прерываем цикл.
             * Детальная причина уже залогирована в app_ftp_send_file(). */
            ESP_LOGW(TAG, "FTP send failed for: %s. Aborting upload session.", filename);
            break;
        }
    }

    return any_uploaded;
}

/* =========================================================================
 * Задача FTP / Wi-Fi менеджера
 * ========================================================================= */

static void app_ftp_task(void *pvParameters)
{
    int        web_retry_count      = 0;
    TickType_t next_web_connect_tick = 0; // Немедленное подключение при старте
    TickType_t last_ftp_check        = xTaskGetTickCount() - pdMS_TO_TICKS(30000); // Немедленный запуск FTP при старте


    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000)); /* Шаг цикла 1 секунда для точного тайминга */

        /* Обрабатываем отложенный запрос на остановку веб-сервера из безопасного контекста задачи.
         * app_webserver_request_stop() мог быть вызван из WiFi event task при потере AP.
         * httpd_stop() вызывается здесь — в безопасном контексте FreeRTOS-задачи. */
        app_webserver_process_stop();

        bool       web_enabled  = app_webserver_is_enabled_in_config();
        TickType_t now          = xTaskGetTickCount();
        bool       is_connected = app_wifi_is_connected();

        /* ================================================================
         * ПРИОРИТЕТ 1: Управление Wi-Fi для веб-сервера
         *
         * Если веб-сервер включён — соединение с AP поддерживается постоянно.
         * При потере связи выполняются попытки переподключения с backoff:
         *   первые 3 попытки — раз в 60 сек, далее — раз в 300 сек.
         * Счётчики сбрасываются при отключении веб-сервера в конфигурации.
         * ================================================================ */
        if (web_enabled) {
            if (is_connected) {
                g_system_checklist.wifi_ok = true;
                web_retry_count       = 0;
                next_web_connect_tick = 0;
            } else {
                g_system_checklist.wifi_ok = false;
                if (now >= next_web_connect_tick) {
                    ESP_LOGW(TAG, "Webserver: attempting WiFi connection (attempt %d)...",
                             web_retry_count + 1);
                    esp_err_t err = app_wifi_connect_sta(
                        g_app_config.wifi_ssid, g_app_config.wifi_passwd, 15000);
                    if (err == ESP_OK) {
                        g_system_checklist.wifi_ok = true;
                        is_connected          = true;
                        web_retry_count       = 0;
                        next_web_connect_tick = 0;
                    } else {
                        web_retry_count++;
                        uint32_t delay_sec = (web_retry_count <= 3) ? 60 : 300;
                        ESP_LOGW(TAG, "WiFi connect failed (attempt %d). Retry in %lu sec.",
                                 web_retry_count, (unsigned long)delay_sec);
                        next_web_connect_tick =
                            xTaskGetTickCount() + pdMS_TO_TICKS(delay_sec * 1000);
                    }
                }
            }
        } else {
            /* Веб-сервер выключен — сбрасываем backoff счётчики */
            web_retry_count       = 0;
            next_web_connect_tick = 0;
            g_system_checklist.wifi_ok = is_connected;
        }

        /* ================================================================
         * ПРИОРИТЕТ 2: Проверка и отправка файлов на FTP
         *
         * Выполняется каждые 30 секунд независимо от состояния веб-сервера.
         * Активный веб-сервер не блокирует и не мешает FTP отгрузке.
         * ================================================================ */
        if (now - last_ftp_check < pdMS_TO_TICKS(30000)) {
            continue;
        }
        /* Если отправка не настроена — пропускаем */
        if (!g_app_config.gps_send && !g_app_config.wifi_send) {
            last_ftp_check = now;
            continue;
        }

        /* Проверяем наличие файлов, готовых к отгрузке */
        if (!has_files_for_upload()) {
            ESP_LOGD(TAG, "FTP check: No completed files in .rd directories. Skipping upload round.");
            last_ftp_check = now;
            continue;
        }


        /* ----------------------------------------------------------------
         * Управление подключением для FTP:
         *
         * Веб-сервер АКТИВЕН:
         *   Используем текущее соединение Wi-Fi. Принудительное переподключение
         *   не выполняем — этим управляет логика приоритета 1 с backoff.
         *   Если AP сейчас недоступен — пропускаем эту итерацию FTP.
         *
         * Веб-сервер ВЫКЛЮЧЕН:
         *   Подключаемся к AP самостоятельно (разовое подключение ради FTP).
         *   После завершения отгрузки — отключаемся.
         * ---------------------------------------------------------------- */
        if (!is_connected) {
            if (web_enabled) {
                /* Backoff активен — не форсируем подключение, пропускаем FTP */
                ESP_LOGD(TAG,
                         "Files in .rd found, but WiFi not connected (webserver backoff). "
                         "Waiting for Wi-Fi...");
                continue;
            } else {
                /* Подключаемся разово для FTP */
                ESP_LOGI(TAG, "Files in .rd found. Connecting to WiFi SSID '%s' for FTP...",
                         g_app_config.wifi_ssid);
                if (app_wifi_connect_sta(g_app_config.wifi_ssid,
                                         g_app_config.wifi_passwd, 15000) != ESP_OK) {
                    ESP_LOGW(TAG, "WiFi connect failed. Skipping FTP upload.");
                    last_ftp_check = now;
                    continue;
                }
                is_connected = true;
            }
        }

        last_ftp_check = now;

        ESP_LOGI(TAG, "Starting FTP upload session to %s:%d",
                 g_app_config.ftp_address, g_app_config.ftp_port);

        /* Отправляем GPS файлы */
        bool any_uploaded = false;
        if (g_app_config.gps_send) {
            if (app_ftp_upload_dir(DIR_GPS_RD, DIR_GPS_SD)) {
                any_uploaded = true;
            }
        }

        /* Отправляем WiFi файлы */
        if (g_app_config.wifi_send) {
            if (app_ftp_upload_dir(DIR_WIFI_RD, DIR_WIFI_SD)) {
                any_uploaded = true;
            }
        }

        if (any_uploaded && g_app_config.ftp_beep) {
            app_buzzer_play(APP_BUZZER_FTP_SUCCESS);
        }

        /* Отключаемся от AP только если веб-сервер выключен
         * (при активном веб-сервере соединение поддерживается постоянно) */
        if (!web_enabled) {
            app_wifi_disconnect_sta();
        }
    }
}

esp_err_t app_ftp_init(void)
{
    xTaskCreatePinnedToCore(app_ftp_task, "ftp_task", 8192, NULL, 2, NULL, 0);
    return ESP_OK;
}
