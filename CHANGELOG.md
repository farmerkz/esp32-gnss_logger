## [1.1.1](https://github.com/farmerkz/esp32-gnss_logger/compare/v1.1.0...v1.1.1) (2026-08-08)


### Bug Fixes

* **webserver:** исправить отображение статуса подключения WiFi ([d316516](https://github.com/farmerkz/esp32-gnss_logger/commit/d316516eb11de29c776bf059747baff3d67ef6aa))
* **wifi:** исправить панику при потере AP — заменить вызов httpd_stop из WiFi event task на отложенный механизм ([c67a0c8](https://github.com/farmerkz/esp32-gnss_logger/commit/c67a0c8e81567020ee1f34e5b3d19c6095b1ea60))
* **wifi:** устранить циклическую перезагрузку при обрыве связи ([7723b10](https://github.com/farmerkz/esp32-gnss_logger/commit/7723b107f5f4b7f4ccb26ba7a869185d6b1b303d))

## [1.1.1-beta.2](https://github.com/farmerkz/esp32-gnss_logger/compare/v1.1.1-beta.1...v1.1.1-beta.2) (2026-08-07)


### Bug Fixes

* **wifi:** устранить циклическую перезагрузку при обрыве связи ([7723b10](https://github.com/farmerkz/esp32-gnss_logger/commit/7723b107f5f4b7f4ccb26ba7a869185d6b1b303d))

## [1.1.1-beta.1](https://github.com/farmerkz/esp32-gnss_logger/compare/v1.1.0...v1.1.1-beta.1) (2026-08-06)


### Bug Fixes

* **wifi:** исправить панику при потере AP — заменить вызов httpd_stop из WiFi event task на отложенный механизм ([c67a0c8](https://github.com/farmerkz/esp32-gnss_logger/commit/c67a0c8e81567020ee1f34e5b3d19c6095b1ea60))

# [1.1.0](https://github.com/farmerkz/esp32-gnss_logger/compare/v1.0.0...v1.1.0) (2026-08-05)


### Features

* интеграция автоматического версионирования в систему сборки и UI ([038adf1](https://github.com/farmerkz/esp32-gnss_logger/commit/038adf17969d7aa41cd520cb36d01a62de7c5eef))

# Changelog

All notable changes to this project will be documented in this file.

## [1.0.0] - 2026-08-05
### Общее
* Инициализация автоматического версионирования проекта. Вся предыдущая история скрыта до внедрения Conventional Commits.
