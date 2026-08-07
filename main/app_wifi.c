/**
 * @file app_wifi.c
 * @brief Реализация работы с драйвером WiFi в режиме Station.
 */

#include "app_wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "WIFI";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Максимальное число автоматических попыток переподключения
#define WIFI_MAX_RETRY     10

static esp_netif_t *s_netif_sta = NULL;
static int  s_retry_count = 0;
// Флаг: true = disconnect вызван намеренно через app_wifi_disconnect_sta()
// Используется для подавления авторевконнекта после явного отключения
static bool s_intentional_disconnect = false;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Не подключаемся автоматически при старте:
        // соединение управляется явно через app_wifi_connect_sta()
        ESP_LOGI(TAG, "WiFi STA started, waiting for explicit connect call.");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_intentional_disconnect) {
            // Намеренное отключение — авторевконнект не нужен
            s_intentional_disconnect = false;
            ESP_LOGI(TAG, "Intentional disconnect, reason: %d", disc->reason);
            return;
        }

        // Неожиданный разрыв — пытаемся переподключиться
        ESP_LOGW(TAG, "Unexpected disconnect, reason: %d (retry %d/%d)",
                 disc->reason, s_retry_count + 1, WIFI_MAX_RETRY);

        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Max reconnect attempts reached.");
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}


esp_err_t app_wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Изначально находимся в состоянии "не подключено / failed",
    // чтобы app_ftp_task мог инициировать первое подключение
    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

    ESP_LOGI(TAG, "WiFi station driver initialized");
    return ESP_OK;
}

esp_err_t app_wifi_connect_sta(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (ssid == NULL || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (app_wifi_is_connected()) {
        ESP_LOGI(TAG, "Already connected to Wi-Fi AP: %s", ssid);
        return ESP_OK;
    }

    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }

    // Порог аутентификации: принимаем WPA2 и WPA3 (совместимый режим)
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    // Поддержка PMF (Protected Management Frames) — обязательно для WPA3-SAE
    wifi_config.sta.pmf_cfg.capable  = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Поддержка WPA3-SAE H2E (Hash-to-Element) и Hunt-and-Peck
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP SSID: %s", ssid);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Failed to connect to AP SSID: %s", ssid);
        return ESP_FAIL;
    }
}

bool app_wifi_is_connected(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void app_wifi_disconnect_sta(void)
{
    s_intentional_disconnect = true; // Подавляем авторевконнект
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "Disconnected from WiFi AP");
}

bool app_wifi_is_failed(void)
{
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_FAIL_BIT) != 0;
}

