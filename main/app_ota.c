/**
 * @file app_ota.c
 * @brief Реализация OTA обновления бинарного файла firmware.bin с SD карты.
 */

#include "app_ota.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "app_sdcard.h"
#include "app_buzzer.h"

#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "OTA";
#define OTA_NVS_NAMESPACE "ota_stat"
#define OTA_NVS_KEY "status"
#define OTA_STATUS_MAGIC 0x07A00002

esp_err_t app_ota_save_status(app_ota_source_t src, bool download_ok, bool first_boot_ok, bool pending_verify, const char *err_msg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", OTA_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    app_ota_status_t st = {
        .magic = OTA_STATUS_MAGIC,
        .source = (uint8_t)src,
        .download_ok = download_ok,
        .first_boot_ok = first_boot_ok,
        .pending_verify = pending_verify,
        .error_msg = {0}
    };
    if (err_msg) {
        strncpy(st.error_msg, err_msg, sizeof(st.error_msg) - 1);
    }

    err = nvs_set_blob(handle, OTA_NVS_KEY, &st, sizeof(st));
    if (err == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Saved OTA status: src=%d, download=%d, boot=%d, pending=%d, err='%s'",
                 src, download_ok, first_boot_ok, pending_verify, st.error_msg);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_ota_get_status(app_ota_status_t *out_status)
{
    if (out_status == NULL) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t size = sizeof(app_ota_status_t);
    err = nvs_get_blob(handle, OTA_NVS_KEY, out_status, &size);
    nvs_close(handle);

    if (err == ESP_OK && out_status->magic == OTA_STATUS_MAGIC) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t app_ota_init(void)
{
    app_ota_status_t st;
    if (app_ota_get_status(&st) == ESP_OK) {
        esp_ota_img_states_t ota_state;
        const esp_partition_t *running = esp_ota_get_running_partition();
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
                ESP_LOGI(TAG, "Running new OTA image pending verification...");
            } else if (st.pending_verify) {
                // Если статус NVS имел pending_verify = true, но текущий раздел не в режиме PENDING_VERIFY — произошел Rollback!
                ESP_LOGW(TAG, "Bootloader rollback detected! Previous OTA firmware failed to boot.");
                app_ota_save_status((app_ota_source_t)st.source, st.download_ok, false, false, "Сбой при первом запуске (выполнен Rollback)");
            }
        }
    }
    return ESP_OK;
}

esp_err_t app_ota_check_and_update(void)
{
    const char *ota_paths[] = { OTA_FIRMWARE_PATH_1, OTA_FIRMWARE_PATH_2 };
    const char *target_path = NULL;
    FILE *f = NULL;

    for (size_t i = 0; i < sizeof(ota_paths) / sizeof(ota_paths[0]); i++) {
        /* M-3: fopen выполняем под g_sd_mutex, так как syslog_task уже запущена */
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        f = fopen(ota_paths[i], "rb");
        xSemaphoreGive(g_sd_mutex);
        if (f != NULL) {
            target_path = ota_paths[i];
            break;
        }
    }

    if (f == NULL || target_path == NULL) {
        return ESP_OK; // Файл обновления отсутствует, нормальный старт
    }

    ESP_LOGI(TAG, "Found firmware update file '%s'! Starting OTA...", target_path);

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        ESP_LOGE(TAG, "Firmware update file is empty!");
        fclose(f);
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Пустой файл обновления");
        return ESP_ERR_INVALID_SIZE;
    }

    // Предварительная проверка заголовка ESP32 бинарника (первый байт должен быть magic 0xE9)
    uint8_t magic = 0;
    if (fread(&magic, 1, 1, f) != 1 || magic != 0xE9) {
        ESP_LOGE(TAG, "Invalid firmware image header (magic byte 0x%02X != 0xE9)!", magic);
        fclose(f);
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Неверный заголовок файла (0xE9)");
        return ESP_ERR_INVALID_RESPONSE;
    }
    fseek(f, 0, SEEK_SET); // Возвращаемся в начало файла

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find valid OTA update partition!");
        fclose(f);
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Раздел OTA не найден");
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_partition, (size_t)fsize, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        fclose(f);
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Ошибка esp_ota_begin");
        return err;
    }

    char buf[1024];
    size_t r = 0;
    size_t written_total = 0;
    bool write_failed = false;

    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write error: %s", esp_err_to_name(err));
            write_failed = true;
            break;
        }
        written_total += r;
    }

    bool file_read_error = ferror(f);
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    fclose(f);
    xSemaphoreGive(g_sd_mutex);

    // Проверка сбоев чтения с SD-карты и полноты вычитки файла
    if (write_failed || file_read_error || written_total != (size_t)fsize) {
        ESP_LOGE(TAG, "OTA update aborted! Read error: %d, written: %zu/%ld",
                 file_read_error, written_total, fsize);
        esp_ota_end(ota_handle);
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Сбой чтения SD / записи в Flash");
        app_buzzer_play(APP_BUZZER_FATAL);
        return ESP_FAIL;
    }

    // esp_ota_end выполняет обязательную валидацию образа (сигнатуры и встроенного SHA-256)
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end validation failed: %s", esp_err_to_name(err));
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Ошибка валидации SHA-256");
        app_buzzer_play(APP_BUZZER_FATAL);
        return err;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition error: %s", esp_err_to_name(err));
        app_ota_save_status(APP_OTA_SRC_SD, false, false, false, "Ошибка выбора загрузочного раздела");
        return err;
    }

    // Сохраняем успешный статус загрузки OTA
    app_ota_save_status(APP_OTA_SRC_SD, true, false, true, "");

    // Удаляем файл обновления после успешной прошивки
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    unlink(target_path);
    xSemaphoreGive(g_sd_mutex);
    ESP_LOGI(TAG, "Firmware successfully updated from SD ('%s')! Total bytes: %zu. Rebooting...", target_path, written_total);

    app_buzzer_play(APP_BUZZER_FTP_SUCCESS);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

esp_err_t app_ota_mark_valid(void)
{
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "New firmware verified successfully! Cancelling rollback...");
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            
            // Обновляем статус OTA в NVS
            app_ota_status_t st;
            if (app_ota_get_status(&st) == ESP_OK) {
                app_ota_save_status((app_ota_source_t)st.source, st.download_ok, true, false, "");
            }
            return err;
        }
    }
    return ESP_OK;
}

void app_ota_mark_invalid_and_reboot(void)
{
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGE(TAG, "Fatal startup error on pending firmware! Rolling back to previous version...");
            app_buzzer_play(APP_BUZZER_FATAL);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    }
    // Если прошивка уже была проверена раньше или нет другой прошивки, делаем обычную перезагрузку
    esp_restart();
}
