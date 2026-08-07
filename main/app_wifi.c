/**
 * @file app_wifi.c
 * @brief Реализация FSM драйвера WiFi в режиме Station.
 */

#include "app_wifi.h"
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "WIFI_FSM";

EventGroupHandle_t g_network_event_group;

// Локальный флаг для обработчика событий
static bool s_is_connected = false;
static bool s_disconnect_expected = false;
static esp_netif_t *s_netif_sta = NULL;

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started.");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_is_connected = false;
        xEventGroupClearBits(g_network_event_group, BIT_WIFI_CONNECTED);
        if (!s_disconnect_expected) {
            ESP_LOGW(TAG, "WiFi disconnected, reason: %d", disc->reason);
        } else {
            ESP_LOGI(TAG, "WiFi intentional disconnect.");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_is_connected = true;
        xEventGroupSetBits(g_network_event_group, BIT_WIFI_CONNECTED);
    }
}

static void app_wifi_task(void *pvParameters)
{
    int retry_count = 0;
    const int MAX_RETRIES = 6;

    for (;;) {
        // Ожидание появления AP от сканера
        ESP_LOGI(TAG, "WIFI_IDLE: Waiting for BIT_AP_AVAILABLE...");
        xEventGroupWaitBits(g_network_event_group, BIT_AP_AVAILABLE,
                            pdFALSE, pdFALSE, portMAX_DELAY);

        ESP_LOGI(TAG, "AP Available detected. Trying to connect...");
        
        wifi_config_t wifi_config;
        memset(&wifi_config, 0, sizeof(wifi_config_t));
        strncpy((char *)wifi_config.sta.ssid, g_app_config.wifi_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, g_app_config.wifi_passwd, sizeof(wifi_config.sta.password) - 1);

        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable  = true;
        wifi_config.sta.pmf_cfg.required = false;
        wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        
        retry_count = 0;
        s_disconnect_expected = false;

        bool locked = false;

        while (retry_count < MAX_RETRIES) {
            // Если AP_AVAILABLE пропал (например, внешне сбросили) - выходим из цикла
            if (!(xEventGroupGetBits(g_network_event_group) & BIT_AP_AVAILABLE)) {
                break;
            }

            if (!s_is_connected) {
                esp_wifi_connect();
                // Ждем либо получения IP (BIT_WIFI_CONNECTED), либо мы сами отвалимся и retry++
                // Для этого ждем 15 секунд на попытку
                EventBits_t bits = xEventGroupWaitBits(g_network_event_group, 
                                                       BIT_WIFI_CONNECTED, 
                                                       pdFALSE, pdFALSE, 
                                                       pdMS_TO_TICKS(15000));
                if (bits & BIT_WIFI_CONNECTED) {
                    retry_count = 0; // Сброс счетчика при успешном получении IP
                    // Ждем разрыва связи или пропажи BIT_AP_AVAILABLE
                    while ((xEventGroupGetBits(g_network_event_group) & BIT_WIFI_CONNECTED) &&
                           (xEventGroupGetBits(g_network_event_group) & BIT_AP_AVAILABLE)) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                } else {
                    retry_count++;
                    ESP_LOGW(TAG, "Connection attempt %d/%d failed.", retry_count, MAX_RETRIES);
                }
            } else {
                // Уже подключены, ждем обрыва связи
                while ((xEventGroupGetBits(g_network_event_group) & BIT_WIFI_CONNECTED) &&
                       (xEventGroupGetBits(g_network_event_group) & BIT_AP_AVAILABLE)) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }

            // Если AP_AVAILABLE пропал - выходим
            if (!(xEventGroupGetBits(g_network_event_group) & BIT_AP_AVAILABLE)) {
                break;
            }

            if (retry_count >= MAX_RETRIES) {
                locked = true;
                break;
            }
        }

        // Отключаемся если еще подключены или пытаемся
        s_disconnect_expected = true;
        esp_wifi_disconnect();
        xEventGroupClearBits(g_network_event_group, BIT_WIFI_CONNECTED);
        s_is_connected = false;

        if (locked) {
            ESP_LOGE(TAG, "WIFI_LOCKED: %d retries failed. Setting BIT_WIFI_FAILED.", MAX_RETRIES);
            xEventGroupSetBits(g_network_event_group, BIT_WIFI_FAILED);
            
            // Ждем пока сканер убедится, что AP исчезла (снимет BIT_AP_AVAILABLE)
            while (xEventGroupGetBits(g_network_event_group) & BIT_AP_AVAILABLE) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            
            // Блокировка снята
            ESP_LOGI(TAG, "AP absence confirmed. Clearing BIT_WIFI_FAILED.");
            xEventGroupClearBits(g_network_event_group, BIT_WIFI_FAILED);
        }
    }
}

esp_err_t app_wifi_init(void)
{
    g_network_event_group = xEventGroupCreate();

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

    xTaskCreatePinnedToCore(app_wifi_task, "wifi_fsm_task", 4096, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "WiFi FSM driver initialized");
    return ESP_OK;
}

bool app_wifi_is_connected(void)
{
    return s_is_connected;
}
