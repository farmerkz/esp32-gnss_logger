/**
 * @file app_ftp.h
 * @brief Легковесный клиент FTP на сокетах BSD для отправки сохраненных файлов.
 */

#ifndef APP_FTP_H
#define APP_FTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Инициализация фоновой задачи проверки и отправки файлов из папок .rd на FTP сервер.
 * @return esp_err_t ESP_OK при успехе.
 */
esp_err_t app_ftp_init(void);

#ifdef __cplusplus
}
#endif

#endif // APP_FTP_H
