/**
 * @file app_config.h
 * @brief Модуль загрузки и хранения параметров конфигурации системы.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_FILE_PATH "/sdcard/config.json" // Имя файла конфигурации на SD
#define APP_CONFIG_MAGIC 0xCFC00001            // Сигнатура валидности NVS конфигурации

/**
 * @brief Структура конфигурации параметров работы системы
 */
typedef struct {
    uint32_t config_magic;       // Сигнатура валидной конфигурации
    char     wifi_ssid[32];      // SSID WiFi точки для загрузки FTP
    char     wifi_passwd[64];    // Пароль WiFi точки
    char     ftp_address[64];    // IP адрес или имя FTP сервера
    char     ftp_user[32];       // Имя пользователя FTP
    char     ftp_passwd[32];     // Пароль пользователя FTP
    uint16_t ftp_port;           // Порт FTP (по умолчанию 21)
    uint32_t filesize_limit;     // Максимальный размер файла WiFi CSV в байтах (по умолчанию 1000000)
    bool     gps_send;           // Флаг отправки файлы треков на FTP
    bool     wifi_send;          // Флаг отправки файлов WiFi на FTP
    bool     ftp_beep;           // Флаг воспроизведения звукового сигнала после FTP
    char     module_id[8];       // Идентификатор устройства (добавляется к именам файлов)
    uint16_t pacc_mask;          // Маска Position Accuracy для GNSS (в метрах)
    uint16_t pdop_mask;          // Маска PDOP для GNSS (*10)
    int8_t   timezone_offset;    // Смещение системного времени в часах от UTC (по умолчанию 5 = UTC+5)
    bool     webserver_enable;   // Флаг разрешения работы веб-сервера (по умолчанию false)
    char     webserver_user[32]; // Имя пользователя для веб-сервера
    char     webserver_passwd[64];// Пароль для доступа к веб-серверу
    uint32_t min_track_size;     // Минимальный размер GPX/CSV файла для переноса в .rd (байт, по умолчанию 1000)
} app_config_t;

#define CONFIG_BACKUP_PATH "/sdcard/config.json.bak" // Путь к бэкапу конфигурации

/**
 * @brief Глобальный экземпляр текущих настроек
 */
extern app_config_t g_app_config;

/**
 * @brief Заполнение структуры конфигурации значениями по умолчанию.
 * @param out_cfg Указатель на структуру для заполнения.
 */
void app_config_get_defaults(app_config_t *out_cfg);

/**
 * @brief Инициализация и гибридная загрузка конфигурации.
 * NVS является первоисточником. При отсутствии NVS читается SD config.json.
 * При наличии разницы между SD config.json и NVS происходит обновление NVS.
 * @return esp_err_t ESP_OK при успехе, ESP_ERR_NOT_FOUND при фатальной ошибке.
 */
esp_err_t app_config_load(void);

/**
 * @brief Чтение параметров конфигурации напрямую из файла на SD карте.
 * @param out_cfg Указатель на структуру для заполнения.
 * @return true если файл успешно прочитан и распарсен, false в противном случае.
 */
bool app_config_read_sd_json(app_config_t *out_cfg);

/**
 * @brief Проверка, загружена ли валидная конфигурация из NVS/SD.
 * @return true если конфигурация валидна.
 */
bool app_config_is_valid(void);

/**
 * @brief Сохранение текущей конфигурации в NVS с отметкой валидности.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_config_save_nvs(void);

/**
 * @brief Валидация JSON структуры конфигурации и сохранение ТОЛЬКО в файл /sdcard/config.json
 * с созданием резервной копии /sdcard/config.json.bak под защитой g_sd_mutex.
 * В файл сохраняются только параметры, отличающиеся от значений по умолчанию.
 * Оперативная конфигурация g_app_config и NVS текущего сеанса НЕ меняются.
 * @param json_str Валидная JSON строка с параметрами конфигурации.
 * @return esp_err_t ESP_OK при успешном сохранении.
 */
esp_err_t app_config_save_json_file_only(const char *json_str);

#ifdef __cplusplus
}
#endif

#endif // APP_CONFIG_H
