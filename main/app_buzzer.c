/**
 * @file app_buzzer.c
 * @brief Реализация звуковой сигнализации с подробным кодированием тональностей.
 */

#include "app_buzzer.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "BUZZER";

/* Мьютекс защищает LEDC-драйвер от конкурентного воспроизведения из разных задач */
SemaphoreHandle_t g_buzzer_mutex = NULL;

#define LEDC_BUZZER_TIMER       LEDC_TIMER_0
#define LEDC_BUZZER_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_BUZZER_CHANNEL     LEDC_CHANNEL_0
#define LEDC_BUZZER_DUTY_RES    LEDC_TIMER_10_BIT // 10-бит разрядность (0..1023)
#define LEDC_BUZZER_DUTY_50     512                // 50% скважность

esp_err_t app_buzzer_init(void)
{
    ESP_LOGI(TAG, "Initializing Buzzer on GPIO %d...", BUZZER_GPIO_PIN);

    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_BUZZER_MODE,
        .duty_resolution = LEDC_BUZZER_DUTY_RES,
        .timer_num       = LEDC_BUZZER_TIMER,
        .freq_hz         = 1000, // Стартовая частота
        .clk_cfg         = LEDC_AUTO_CLK
    };
    esp_err_t err = ledc_timer_config(&timer_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t chan_conf = {
        .gpio_num   = BUZZER_GPIO_PIN,
        .speed_mode = LEDC_BUZZER_MODE,
        .channel    = LEDC_BUZZER_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_BUZZER_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    err = ledc_channel_config(&chan_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Создаём мьютекс для сериализации доступа к LEDC */
    g_buzzer_mutex = xSemaphoreCreateMutex();
    if (g_buzzer_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create buzzer mutex!");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void app_buzzer_play_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    if (freq_hz == 0) {
        ledc_set_duty(LEDC_BUZZER_MODE, LEDC_BUZZER_CHANNEL, 0);
        ledc_update_duty(LEDC_BUZZER_MODE, LEDC_BUZZER_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(duration_ms));
        return;
    }

    ledc_set_freq(LEDC_BUZZER_MODE, LEDC_BUZZER_TIMER, freq_hz);
    ledc_set_duty(LEDC_BUZZER_MODE, LEDC_BUZZER_CHANNEL, LEDC_BUZZER_DUTY_50);
    ledc_update_duty(LEDC_BUZZER_MODE, LEDC_BUZZER_CHANNEL);

    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    ledc_set_duty(LEDC_BUZZER_MODE, LEDC_BUZZER_CHANNEL, 0);
    ledc_update_duty(LEDC_BUZZER_MODE, LEDC_BUZZER_CHANNEL);
}

void app_buzzer_play(app_buzzer_sound_t sound)
{
    /* Захватываем мьютекс: предотвращаем одновременное воспроизведение из разных задач */
    if (g_buzzer_mutex != NULL) {
        xSemaphoreTake(g_buzzer_mutex, portMAX_DELAY);
    }

    switch (sound) {
    case APP_BUZZER_STARTUP:
        // Двойной восходящий тон
        app_buzzer_play_tone(1000, 120);
        vTaskDelay(pdMS_TO_TICKS(80));
        app_buzzer_play_tone(1500, 160);
        break;

    case APP_BUZZER_FILE_ROTATE:
        // Короткий тихий клик
        app_buzzer_play_tone(2000, 20);
        break;

    case APP_BUZZER_FTP_SUCCESS:
        // Мажорный аккорд
        app_buzzer_play_tone(1000, 120);
        vTaskDelay(pdMS_TO_TICKS(30));
        app_buzzer_play_tone(1250, 120);
        vTaskDelay(pdMS_TO_TICKS(30));
        app_buzzer_play_tone(1500, 200);
        break;

    case APP_BUZZER_FATAL:
        // Прерывистый тревожный тон (5 повторений)
        for (int i = 0; i < 5; i++) {
            app_buzzer_play_tone(800, 250);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        break;

    default:
        break;
    }

    /* Освобождаем мьютекс после окончания воспроизведения */
    if (g_buzzer_mutex != NULL) {
        xSemaphoreGive(g_buzzer_mutex);
    }
}
