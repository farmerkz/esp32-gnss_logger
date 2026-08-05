cmake_minimum_required(VERSION 3.16)

# Читаем версию из файла, сгенерированного CI/CD
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/version.txt")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" APP_VERSION)
    string(STRIP "${APP_VERSION}" APP_VERSION) # Удаляем невидимые символы и переносы строк
else()
    # Запасной вариант для локальной сборки до первого релиза
    set(APP_VERSION "0.0.0-dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Инициализируем проект с передачей актуальной версии для встроенных механизмов (например, OTA)
set(PROJECT_VER "${APP_VERSION}")
project(esp32-gnss_logger)

# Добавляем глобальный макрос FIRMWARE_VERSION, доступный в любом .c/.cpp файле проекта
add_compile_definitions(FIRMWARE_VERSION="${APP_VERSION}")
