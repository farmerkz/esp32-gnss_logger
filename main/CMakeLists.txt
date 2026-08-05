idf_component_register(
    SRCS 
        "main.c"
        "app_buzzer.c"
        "app_config.c"
        "app_sdcard.c"
        "app_gnss.c"
        "app_gpx.c"
        "app_wigle.c"
        "app_wifi.c"
        "app_ftp.c"
        "app_ota.c"
        "app_log_buffer.c"
        "app_webserver.c"
    INCLUDE_DIRS "."
    REQUIRES 
        driver
        esp_driver_ledc
        esp_driver_uart
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        esp_timer
        mbedtls
        cjson
        app_update
        fatfs
        vfs
        ubx_parser
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE FIRMWARE_VERSION="${PROJECT_VER}")
