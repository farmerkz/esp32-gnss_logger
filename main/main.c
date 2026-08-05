/**
 * @file main.c
 * @brief Главная точка входа приложения ESP32 GNSS Logger & Wardriving на ESP-IDF.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "app_buzzer.h"
#include "app_config.h"
#include "app_sdcard.h"
#include "app_gnss.h"
#include "app_gpx.h"
#include "app_wigle.h"
#include "app_wifi.h"
#include "app_ftp.h"
#include "app_ota.h"
#include "app_log_buffer.h"
#include "app_webserver.h"
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "MAIN";

#define SYSLOG_FILE_PATH "/sdcard/syslog.txt"
#define SYSLOG_FILE_BAK  "/sdcard/syslog.bak.txt"
#define SYSLOG_MAX_SIZE  (10 * 1024 * 1024)

static void app_syslog_task(void *pvParameters)
{
    char *temp_buf = malloc(2048);
    if (!temp_buf) {
        ESP_LOGE(TAG, "Failed to allocate syslog temp buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        size_t bytes = app_log_buffer_read_for_sd(temp_buf, 2047);
        if (bytes > 0) {
            temp_buf[bytes] = '\0';

            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            struct stat st;
            if (stat(SYSLOG_FILE_PATH, &st) == 0) {
                if (st.st_size > SYSLOG_MAX_SIZE) {
                    unlink(SYSLOG_FILE_BAK);
                    rename(SYSLOG_FILE_PATH, SYSLOG_FILE_BAK);
                }
            }

            FILE *f = fopen(SYSLOG_FILE_PATH, "a");
            if (f) {
                fwrite(temp_buf, 1, bytes, f);
                fclose(f);
            }
            xSemaphoreGive(g_sd_mutex);
        }
    }
}

void app_main(void)
{
    // 1. Инициализация буфера логов в DRAM
    app_log_buffer_init();

    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "Starting ESP32 GNSS Logger & Wardriving (ESP-IDF)");
    ESP_LOGI(TAG, "Firmware Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "=============================================");

    // Определение и вывод причины перезагрузки
    esp_reset_reason_t reason = esp_reset_reason();
    const char *reason_str = "Unknown";
    switch (reason) {
        case ESP_RST_POWERON: reason_str = "Power-on"; break;
        case ESP_RST_EXT: reason_str = "External pin"; break;
        case ESP_RST_SW: reason_str = "Software reset"; break;
        case ESP_RST_PANIC: reason_str = "Hardware exception / Panic"; break;
        case ESP_RST_INT_WDT: reason_str = "Interrupt Watchdog"; break;
        case ESP_RST_TASK_WDT: reason_str = "Task Watchdog"; break;
        case ESP_RST_WDT: reason_str = "Other Watchdog"; break;
        case ESP_RST_DEEPSLEEP: reason_str = "Deep sleep wake-up"; break;
        case ESP_RST_BROWNOUT: reason_str = "Brownout"; break;
        case ESP_RST_SDIO: reason_str = "SDIO reset"; break;
        default: reason_str = "Unknown"; break;
    }
    ESP_LOGI(TAG, "Reset reason: %s (%d)", reason_str, reason);

    // 2. Инициализация звуковой сигнализации (Buzzer)
    if (app_buzzer_init() == ESP_OK) {
        app_buzzer_play(APP_BUZZER_STARTUP);
    }

    // 3. Инициализация энергонезависимой памяти NVS Flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Инициализация OTA модуля и проверка Rollback
    app_ota_init();

    // 4. Инициализация драйверов WiFi
    app_wifi_init();

    // 5. Пробуем смонтировать SD карту для возможного чтения config.json
    bool sd_mounted = (app_sdcard_init() == ESP_OK);
    g_system_checklist.sd_card_ok = sd_mounted;

    if (sd_mounted) {
        xTaskCreatePinnedToCore(app_syslog_task, "syslog_task", 4096, NULL, 2, NULL, 1);
        ESP_LOGI(TAG, "Syslog SD task started");
    }

    // 6. Гибридная загрузка конфигурации (NVS primary + SD file sync)
    esp_err_t cfg_err = app_config_load();
    if (cfg_err != ESP_OK) {
        g_system_checklist.nvs_config_ok = false;
        ESP_LOGE(TAG, "FATAL: No valid configuration in NVS and failed to load config.json from SD card!");
        app_buzzer_play(APP_BUZZER_FATAL);
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
        return;
    }
    g_system_checklist.nvs_config_ok = true;

    // 7. Проверка наличия файла обновления прошивки на SD-карте (firmware.bin или esp32-gnss_logger.bin)
    if (sd_mounted) {
        app_ota_check_and_update();
    }

    // 7. Проверка условий активации Web-сервера
    bool web_active = app_webserver_is_enabled_in_config();

    // 8. Инициализация и проверка GNSS модуля
    bool gnss_ok = (app_gnss_init() == ESP_OK);
    g_system_checklist.gnss_ok = gnss_ok;

    // 9. Запуск подсистем и управление ошибками оборудования
    if (web_active) {
        ESP_LOGI(TAG, "Webserver activation enabled in configuration.");
        // Подключаемся к AP (non-fatal при сбое, двухфазный реконнект в app_ftp)
        app_wifi_connect_sta(g_app_config.wifi_ssid, g_app_config.wifi_passwd, 5000);

        // Всегда запускаем фоновую задачу FTP / реконнекта Wi-Fi
        app_ftp_init();

        if (!sd_mounted || !gnss_ok) {
            ESP_LOGW(TAG, "WARNING: Hardware error detected (SD: %s, GNSS: %s). Tracks & Wardriving disabled. Web Diagnostics active.",
                     sd_mounted ? "OK" : "FAIL", gnss_ok ? "OK" : "FAIL");
            // Устройство остается работать в режиме веб-диагностики, логов и OTA
            app_ota_mark_valid();
        } else {
            ESP_LOGI(TAG, "All hardware checks passed. Launching GPX tracker (Wardriving disabled in webserver mode)...");
            ESP_LOGW(TAG, "R-1: WiFi scanning (Wardriving) is DISABLED while webserver is active.");
            app_sdcard_cleanup_work_dirs();
            app_gpx_init();  /* R-1: GPX треки пишутся всегда */
            /* app_wigle_init() НЕ вызывается: сканирование WiFi несовместимо с активным веб-сервером */
            app_ota_mark_valid();
        }
    } else {
        ESP_LOGI(TAG, "Webserver activation disabled.");
        if (!sd_mounted || !gnss_ok) {
            ESP_LOGE(TAG, "FATAL: Hardware failure (SD: %s, GNSS: %s) with Webserver disabled! Rebooting...",
                     sd_mounted ? "OK" : "FAIL", gnss_ok ? "OK" : "FAIL");
            app_buzzer_play(APP_BUZZER_FATAL);
            app_ota_mark_invalid_and_reboot();
            return;
        } else {
            ESP_LOGI(TAG, "All hardware checks passed. Launching GPX tracker, Wigle scanner and FTP worker...");
            app_sdcard_cleanup_work_dirs();
            app_gpx_init();
            app_wigle_init();
            app_ftp_init();
            app_ota_mark_valid();
        }
    }

    ESP_LOGI(TAG, "System initialization complete. Entering monitoring loop.");

    // Главный цикл мониторинга ресурсов
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(600000));
        ESP_LOGI(TAG, "System Health Check - Free Internal Heap: %lu bytes",
                 (unsigned long)esp_get_free_internal_heap_size());
    }
}
