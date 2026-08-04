/**
 * @file app_config.c
 * @brief Реализация загрузки настроек из JSON cJSON и гибридного резервирования в NVS.
 */

#include "app_config.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include "app_sdcard.h"

static const char *TAG = "CONFIG";

app_config_t g_app_config;

void app_config_get_defaults(app_config_t *cfg)
{
    memset(cfg, 0, sizeof(app_config_t));
    cfg->config_magic = APP_CONFIG_MAGIC;
    strncpy(cfg->wifi_ssid, "ssid", sizeof(cfg->wifi_ssid) - 1);
    strncpy(cfg->wifi_passwd, "wifipassword", sizeof(cfg->wifi_passwd) - 1);
    strncpy(cfg->ftp_address, "192.168.168.100", sizeof(cfg->ftp_address) - 1);
    strncpy(cfg->ftp_user, "ftpuser", sizeof(cfg->ftp_user) - 1);
    strncpy(cfg->ftp_passwd, "ftppasswd", sizeof(cfg->ftp_passwd) - 1);
    cfg->ftp_port = 21;
    cfg->filesize_limit = 1000000UL;
    cfg->gps_send = true;
    cfg->wifi_send = true;
    cfg->ftp_beep = false;
    strncpy(cfg->module_id, "00", sizeof(cfg->module_id) - 1);
    cfg->pacc_mask = 0;
    cfg->pdop_mask = 0;
    cfg->timezone_offset = 5; // По умолчанию UTC+5
    cfg->webserver_enable = false;
    memset(cfg->webserver_user, 0, sizeof(cfg->webserver_user));
    memset(cfg->webserver_passwd, 0, sizeof(cfg->webserver_passwd));
    cfg->min_track_size = 1000; // Минимальный размер файла для переноса (байт)
}

static void app_config_set_defaults(app_config_t *cfg)
{
    app_config_get_defaults(cfg);
}

bool app_config_is_valid(void)
{
    return (g_app_config.config_magic == APP_CONFIG_MAGIC);
}

esp_err_t app_config_save_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    g_app_config.config_magic = APP_CONFIG_MAGIC;
    nvs_set_blob(handle, "cfg_blob", &g_app_config, sizeof(app_config_t));
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Configuration saved & validated in NVS storage (size=%u)", (unsigned)sizeof(app_config_t));
    return ESP_OK;
}

static bool app_config_load_nvs(app_config_t *out_cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("config", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t stored_size = 0;
    err = nvs_get_blob(handle, "cfg_blob", NULL, &stored_size);
    if (err != ESP_OK || stored_size == 0) {
        nvs_close(handle);
        return false;
    }

    // Заполняем дефолтными значениями перед чтением блоба для бесшовной OTA-миграции
    app_config_get_defaults(out_cfg);

    size_t read_size = (stored_size > sizeof(app_config_t)) ? sizeof(app_config_t) : stored_size;
    err = nvs_get_blob(handle, "cfg_blob", out_cfg, &read_size);
    nvs_close(handle);

    if (err == ESP_OK && out_cfg->config_magic == APP_CONFIG_MAGIC) {
        if (stored_size != sizeof(app_config_t)) {
            ESP_LOGW(TAG, "OTA NVS structure size migration: stored=%u, current=%u. Upgrading NVS...",
                     (unsigned)stored_size, (unsigned)sizeof(app_config_t));
            // Записываем обновлённую структуру в NVS
            g_app_config = *out_cfg;
            app_config_save_nvs();
        } else {
            ESP_LOGI(TAG, "Valid configuration restored from NVS");
        }
        return true;
    }
    return false;
}

static bool parse_json_bool(const cJSON *item, bool *out_val)
{
    if (!item) return false;
    if (cJSON_IsBool(item)) {
        *out_val = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out_val = (item->valueint != 0);
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring) {
        if (strcasecmp(item->valuestring, "true") == 0 || strcmp(item->valuestring, "1") == 0) {
            *out_val = true;
            return true;
        }
        if (strcasecmp(item->valuestring, "false") == 0 || strcmp(item->valuestring, "0") == 0) {
            *out_val = false;
            return true;
        }
    }
    return false;
}

static bool app_config_parse_json(const char *json_str, app_config_t *cfg)
{
    cJSON *json = cJSON_Parse(json_str);
    if (json == NULL) {
        ESP_LOGE(TAG, "cJSON parse error in config file");
        return false;
    }

    cJSON *item = NULL;
    if ((item = cJSON_GetObjectItem(json, "ssid")) && cJSON_IsString(item)) {
        strncpy(cfg->wifi_ssid, item->valuestring, sizeof(cfg->wifi_ssid) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "wifipasswd")) && cJSON_IsString(item)) {
        strncpy(cfg->wifi_passwd, item->valuestring, sizeof(cfg->wifi_passwd) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "ftpaddress")) && cJSON_IsString(item)) {
        strncpy(cfg->ftp_address, item->valuestring, sizeof(cfg->ftp_address) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "ftpuser")) && cJSON_IsString(item)) {
        strncpy(cfg->ftp_user, item->valuestring, sizeof(cfg->ftp_user) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "ftppassword")) && cJSON_IsString(item)) {
        strncpy(cfg->ftp_passwd, item->valuestring, sizeof(cfg->ftp_passwd) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "ftpport")) && cJSON_IsNumber(item)) {
        cfg->ftp_port = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "filesize")) && cJSON_IsNumber(item)) {
        cfg->filesize_limit = (uint32_t)item->valuedouble;
    }
    if ((item = cJSON_GetObjectItem(json, "gpssend"))) {
        parse_json_bool(item, &cfg->gps_send);
    }
    if ((item = cJSON_GetObjectItem(json, "wifisend"))) {
        parse_json_bool(item, &cfg->wifi_send);
    }
    if ((item = cJSON_GetObjectItem(json, "ftpbeep"))) {
        parse_json_bool(item, &cfg->ftp_beep);
    }
    if ((item = cJSON_GetObjectItem(json, "moduleid")) && cJSON_IsString(item)) {
        strncpy(cfg->module_id, item->valuestring, sizeof(cfg->module_id) - 1);
    }
    if ((item = cJSON_GetObjectItem(json, "pdopmask")) && cJSON_IsNumber(item)) {
        cfg->pdop_mask = (uint16_t)item->valueint;
    }
    if ((item = cJSON_GetObjectItem(json, "paccmask")) && cJSON_IsNumber(item)) {
        cfg->pacc_mask = (uint16_t)item->valueint;
    }

    cJSON *tz_item = cJSON_GetObjectItem(json, "timezone");
    if (!tz_item) tz_item = cJSON_GetObjectItem(json, "timezone_offset");
    if (!tz_item) tz_item = cJSON_GetObjectItem(json, "tz");
    if (tz_item && cJSON_IsNumber(tz_item)) {
        cfg->timezone_offset = (int8_t)tz_item->valueint;
    }

    cJSON *ws_enable = cJSON_GetObjectItem(json, "webserverenable");
    if (!ws_enable) ws_enable = cJSON_GetObjectItem(json, "webserver_enable");
    if (ws_enable) {
        parse_json_bool(ws_enable, &cfg->webserver_enable);
    }

    cJSON *ws_user = cJSON_GetObjectItem(json, "webuser");
    if (!ws_user) ws_user = cJSON_GetObjectItem(json, "webserver_user");
    if (ws_user && cJSON_IsString(ws_user)) {
        strncpy(cfg->webserver_user, ws_user->valuestring, sizeof(cfg->webserver_user) - 1);
    }

    cJSON *ws_pass = cJSON_GetObjectItem(json, "webpassword");
    if (!ws_pass) ws_pass = cJSON_GetObjectItem(json, "webserver_passwd");
    if (ws_pass && cJSON_IsString(ws_pass)) {
        strncpy(cfg->webserver_passwd, ws_pass->valuestring, sizeof(cfg->webserver_passwd) - 1);
    }

    /* min_track_size: минимальный размер файла для переноса из .wk в .rd */
    cJSON *mts_item = cJSON_GetObjectItem(json, "min_track_size");
    if (!mts_item) mts_item = cJSON_GetObjectItem(json, "mintracksize");
    if (mts_item && cJSON_IsNumber(mts_item) && mts_item->valuedouble > 0) {
        cfg->min_track_size = (uint32_t)mts_item->valuedouble;
    }

    cJSON_Delete(json);
    return true;
}

bool app_config_read_sd_json(app_config_t *out_cfg)
{
    if (g_sd_mutex) xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE *f = fopen(CONFIG_FILE_PATH, "r");
    if (f == NULL) {
        if (g_sd_mutex) xSemaphoreGive(g_sd_mutex);
        ESP_LOGI(TAG, "Config file %s not found", CONFIG_FILE_PATH);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 8192) {
        fclose(f);
        if (g_sd_mutex) xSemaphoreGive(g_sd_mutex);
        ESP_LOGE(TAG, "Invalid config file size: %ld", fsize);
        return false;
    }

    char *buf = (char *)malloc(fsize + 1);
    if (buf == NULL) {
        fclose(f);
        if (g_sd_mutex) xSemaphoreGive(g_sd_mutex);
        ESP_LOGE(TAG, "Memory allocation failed for config read");
        return false;
    }

    size_t read_bytes = fread(buf, 1, fsize, f);
    fclose(f);
    if (g_sd_mutex) xSemaphoreGive(g_sd_mutex);
    
    ESP_LOGI(TAG, "Successfully read %zu bytes from %s", read_bytes, CONFIG_FILE_PATH);
    
    buf[read_bytes] = '\0';

    bool parsed = app_config_parse_json(buf, out_cfg);
    free(buf);
    return parsed;
}

esp_err_t app_config_load(void)
{
    app_config_t nvs_cfg;
    app_config_set_defaults(&nvs_cfg);
    bool has_nvs = app_config_load_nvs(&nvs_cfg);

    app_config_t sd_cfg;
    app_config_set_defaults(&sd_cfg);
    bool has_sd = app_config_read_sd_json(&sd_cfg);

    if (!has_nvs) {
        if (has_sd) {
            ESP_LOGI(TAG, "No valid NVS config found. Primary initialization from SD card config.json...");
            g_app_config = sd_cfg;
            app_config_save_nvs();
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "FATAL: Valid NVS config absent AND SD card config.json is unreadable/missing!");
            memset(&g_app_config, 0, sizeof(app_config_t));
            return ESP_ERR_NOT_FOUND;
        }
    }

    // В NVS есть валидная конфигурация: она является основной
    g_app_config = nvs_cfg;

    if (has_sd) {
        // Сравниваем конфигурации
        if (memcmp(&nvs_cfg, &sd_cfg, sizeof(app_config_t)) != 0) {
            ESP_LOGI(TAG, "Config difference detected between SD card and NVS. Updating NVS config from SD file...");
            g_app_config = sd_cfg;
            app_config_save_nvs();
        } else {
            ESP_LOGI(TAG, "NVS config is up-to-date with SD file.");
        }
    } else {
        ESP_LOGI(TAG, "SD config file unreadable/absent. Operating on valid NVS configuration.");
    }

    return ESP_OK;
}

esp_err_t app_config_save_json_file_only(const char *json_str)
{
    if (json_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_t def_cfg;
    app_config_get_defaults(&def_cfg);

    app_config_t parsed_cfg = def_cfg;
    if (!app_config_parse_json(json_str, &parsed_cfg)) {
        ESP_LOGE(TAG, "Failed to parse JSON string for SD file save");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *filtered = cJSON_CreateObject();
    if (filtered == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (strcmp(parsed_cfg.wifi_ssid, def_cfg.wifi_ssid) != 0) {
        cJSON_AddStringToObject(filtered, "ssid", parsed_cfg.wifi_ssid);
    }
    if (strcmp(parsed_cfg.wifi_passwd, def_cfg.wifi_passwd) != 0) {
        cJSON_AddStringToObject(filtered, "wifipasswd", parsed_cfg.wifi_passwd);
    }
    if (strcmp(parsed_cfg.ftp_address, def_cfg.ftp_address) != 0) {
        cJSON_AddStringToObject(filtered, "ftpaddress", parsed_cfg.ftp_address);
    }
    if (strcmp(parsed_cfg.ftp_user, def_cfg.ftp_user) != 0) {
        cJSON_AddStringToObject(filtered, "ftpuser", parsed_cfg.ftp_user);
    }
    if (strcmp(parsed_cfg.ftp_passwd, def_cfg.ftp_passwd) != 0) {
        cJSON_AddStringToObject(filtered, "ftppassword", parsed_cfg.ftp_passwd);
    }
    if (parsed_cfg.ftp_port != def_cfg.ftp_port) {
        cJSON_AddNumberToObject(filtered, "ftpport", parsed_cfg.ftp_port);
    }
    if (parsed_cfg.filesize_limit != def_cfg.filesize_limit) {
        cJSON_AddNumberToObject(filtered, "filesize", parsed_cfg.filesize_limit);
    }
    if (parsed_cfg.gps_send != def_cfg.gps_send) {
        cJSON_AddBoolToObject(filtered, "gpssend", parsed_cfg.gps_send);
    }
    if (parsed_cfg.wifi_send != def_cfg.wifi_send) {
        cJSON_AddBoolToObject(filtered, "wifisend", parsed_cfg.wifi_send);
    }
    if (parsed_cfg.ftp_beep != def_cfg.ftp_beep) {
        cJSON_AddBoolToObject(filtered, "ftpbeep", parsed_cfg.ftp_beep);
    }
    if (strcmp(parsed_cfg.module_id, def_cfg.module_id) != 0) {
        cJSON_AddStringToObject(filtered, "moduleid", parsed_cfg.module_id);
    }
    if (parsed_cfg.pacc_mask != def_cfg.pacc_mask) {
        cJSON_AddNumberToObject(filtered, "paccmask", parsed_cfg.pacc_mask);
    }
    if (parsed_cfg.pdop_mask != def_cfg.pdop_mask) {
        cJSON_AddNumberToObject(filtered, "pdopmask", parsed_cfg.pdop_mask);
    }
    if (parsed_cfg.timezone_offset != def_cfg.timezone_offset) {
        cJSON_AddNumberToObject(filtered, "timezone", parsed_cfg.timezone_offset);
    }
    if (parsed_cfg.webserver_enable != def_cfg.webserver_enable) {
        cJSON_AddBoolToObject(filtered, "webserverenable", parsed_cfg.webserver_enable);
    }
    if (strcmp(parsed_cfg.webserver_user, def_cfg.webserver_user) != 0) {
        cJSON_AddStringToObject(filtered, "webuser", parsed_cfg.webserver_user);
    }
    if (strcmp(parsed_cfg.webserver_passwd, def_cfg.webserver_passwd) != 0) {
        cJSON_AddStringToObject(filtered, "webpassword", parsed_cfg.webserver_passwd);
    }

    char *formatted_json = cJSON_Print(filtered);
    cJSON_Delete(filtered);

    if (formatted_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (g_sd_mutex != NULL) {
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    }

    // 1. Создание резервной копии: чтение имеющегося файла и сохранение в .bak
    FILE *f_old = fopen(CONFIG_FILE_PATH, "r");
    if (f_old != NULL) {
        fseek(f_old, 0, SEEK_END);
        long sz = ftell(f_old);
        fseek(f_old, 0, SEEK_SET);
        if (sz > 0 && sz < 16384) {
            char *old_buf = (char *)malloc(sz + 1);
            if (old_buf != NULL) {
                size_t read_bytes = fread(old_buf, 1, sz, f_old);
                old_buf[read_bytes] = '\0';
                fclose(f_old);
                f_old = NULL;

                FILE *f_bak = fopen(CONFIG_BACKUP_PATH, "w");
                if (f_bak != NULL) {
                    fwrite(old_buf, 1, read_bytes, f_bak);
                    fflush(f_bak);
                    fclose(f_bak);
                    ESP_LOGI(TAG, "Backup created successfully at %s", CONFIG_BACKUP_PATH);
                }
                free(old_buf);
            }
        }
        if (f_old != NULL) {
            fclose(f_old);
        }
    }

    // 2. Сохранение нового файла конфигурации (только отфильтрованные параметры)
    FILE *f_new = fopen(CONFIG_FILE_PATH, "w");
    if (f_new == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", CONFIG_FILE_PATH);
        if (g_sd_mutex != NULL) {
            xSemaphoreGive(g_sd_mutex);
        }
        cJSON_free(formatted_json);
        return ESP_FAIL;
    }

    fwrite(formatted_json, 1, strlen(formatted_json), f_new);
    fflush(f_new);
    fclose(f_new);
    cJSON_free(formatted_json);

    if (g_sd_mutex != NULL) {
        xSemaphoreGive(g_sd_mutex);
    }

    ESP_LOGI(TAG, "Configuration saved ONLY to SD file %s", CONFIG_FILE_PATH);
    return ESP_OK;
}

