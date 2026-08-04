/**
 * @file ubx_parser.h
 * @brief Легковесная библиотека для парсинга и генерации бинарных сообщений u-blox UBX.
 *
 * Предназначена для использования в встраиваемых системах (ESP-IDF) без динамического
 * выделения памяти в процессе работы. Парсинг выполняется потоково с помощью
 * конечного автомата (FSM).
 */

#ifndef UBX_PARSER_H
#define UBX_PARSER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ====================================================================================
// Синхробайты протокола UBX
// ====================================================================================
#define UBX_SYNC_CHAR1 0xB5
#define UBX_SYNC_CHAR2 0x62

// ====================================================================================
// Классы сообщений UBX
// ====================================================================================
#define UBX_CLASS_NAV 0x01 // Сообщения навигации
#define UBX_CLASS_ACK 0x05 // Сообщения подтверждения (ACK/NAK)
#define UBX_CLASS_CFG 0x06 // Сообщения конфигурации
#define UBX_CLASS_MON 0x0A // Сообщения мониторинга системы

// ====================================================================================
// Идентификаторы (ID) сообщений
// ====================================================================================
#define UBX_NAV_DOP 0x04  // Dilution of precision
#define UBX_NAV_PVT 0x07  // Position, Velocity, Time
#define UBX_ACK_NAK 0x00  // Ошибка выполнения команды
#define UBX_ACK_ACK 0x01  // Успешное выполнение команды
#define UBX_CFG_PRT 0x00  // Конфигурация портов ввода/вывода
#define UBX_CFG_MSG 0x01  // Конфигурация частоты сообщений
#define UBX_CFG_RATE 0x08 // Конфигурация частоты навигации
#define UBX_CFG_NAV5 0x24 // Конфигурация параметров навигации (маски PDOP/PACC)
#define UBX_MON_VER 0x04  // Запрос версии прошивки/модуля

// Максимальный размер полезной нагрузки (payload) для парсера
#define UBX_MAX_PAYLOAD_SIZE 256

/**
 * @brief Состояния конечного автомата (FSM) парсера UBX
 */
typedef enum {
    UBX_STATE_IDLE = 0,    // Ожидание первого синхробайта (0xB5)
    UBX_STATE_SYNC2,       // Ожидание второго синхробайта (0x62)
    UBX_STATE_CLASS,       // Чтение класса сообщения
    UBX_STATE_ID,          // Чтение ID сообщения
    UBX_STATE_LEN_LSB,     // Чтение младшего байта длины payload
    UBX_STATE_LEN_MSB,     // Чтение старшего байта длины payload
    UBX_STATE_PAYLOAD,     // Чтение полезной нагрузки
    UBX_STATE_CK_A,        // Проверка первой контрольной суммы (CK_A)
    UBX_STATE_CK_B         // Проверка второй контрольной суммы (CK_B)
} ubx_state_t;

/**
 * @brief Перечисление результатов обработки очередного байта
 */
typedef enum {
    UBX_PARSE_BUSY = 0,     // Пакет ещё не сформирован полностью
    UBX_PARSE_GOT_PVT,     // Успешно принят и проверен пакет UBX-NAV-PVT
    UBX_PARSE_GOT_DOP,     // Успешно принят и проверен пакет UBX-NAV-DOP
    UBX_PARSE_GOT_ACK,     // Успешно принят ACK на конфигурационную команду
    UBX_PARSE_GOT_NAK,     // Принят NAK (отклонение конфигурационной команды)
    UBX_PARSE_GOT_MON_VER, // Принят ответ на запрос версии UBX-MON-VER
    UBX_PARSE_GOT_OTHER,   // Принят другой валидный пакет UBX
    UBX_PARSE_ERROR        // Ошибка контрольной суммы или переполнение
} ubx_parse_result_t;

/**
 * @brief Упакованная структура пакета UBX-NAV-PVT (92 байта)
 */
typedef struct __attribute__((packed)) {
    uint32_t iTOW;    // Время недели в мс (GPS time of week)
    uint16_t year;    // Год UTC (1999..2099)
    uint8_t  month;   // Месяц UTC (1..12)
    uint8_t  day;     // День UTC (1..31)
    uint8_t  hour;    // Час UTC (0..23)
    uint8_t  min;     // Минута UTC (0..59)
    uint8_t  sec;     // Секунда UTC (0..60)
    uint8_t  valid;   // Флаги валидности даты/времени (bit0: validDate, bit1: validTime, bit2: fullyResolved)
    uint32_t tAcc;    // Точность времени (нс)
    int32_t  nano;    // Дробная часть секунды (нс)
    uint8_t  fixType; // Тип фиксации (0: No fix, 2: 2D, 3: 3D, 4: GNSS+dead reckoning)
    uint8_t  flags;   // Флаги навигации (bit0: gnssFixOK)
    uint8_t  flags2;  // Дополнительные флаги
    uint8_t  numSV;   // Количество спутников, используемых в решении
    int32_t  lon;     // Долгота (градусы * 1e-7)
    int32_t  lat;     // Широта (градусы * 1e-7)
    int32_t  height;  // Высота над эллипсоидом (мм)
    int32_t  hMSL;    // Высота над уровнем моря (мм)
    uint32_t hAcc;    // Горизонтальная точность (мм)
    uint32_t vAcc;    // Вертикальная точность (мм)
    int32_t  velN;    // Скорость на север (мм/с)
    int32_t  velE;    // Скорость на восток (мм/с)
    int32_t  velD;    // Скорость вниз (мм/с)
    int32_t  gSpeed;  // Путевая скорость (2D) (мм/с)
    int32_t  headMot; // Направление движения (градусы * 1e-5)
    uint32_t sAcc;    // Точность скорости (мм/с)
    uint32_t headAcc; // Точность направления (градусы * 1e-5)
    uint16_t pDOP;    // Погрешность снижения точности позиции (PDOP * 0.01)
    uint16_t flags3;  // Дополнительные флаги
    uint8_t  reserved1[4];
    int32_t  headVeh; // Направление транспортного средства (градусы * 1e-5)
    int16_t  magDec;  // Магнитное склонение (градусы * 1e-2)
    uint16_t magAcc;  // Точность магнитного склонения (градусы * 1e-2)
} ubx_nav_pvt_t;

/**
 * @brief Упакованная структура пакета UBX-NAV-DOP (18 байт)
 */
typedef struct __attribute__((packed)) {
    uint32_t iTOW; // GPS time of week (мс)
    uint16_t gDOP; // Geometric DOP (0.01)
    uint16_t pDOP; // Position DOP (0.01)
    uint16_t tDOP; // Time DOP (0.01)
    uint16_t vDOP; // Vertical DOP (0.01)
    uint16_t hDOP; // Horizontal DOP (0.01)
    uint16_t nDOP; // Northing DOP (0.01)
    uint16_t eDOP; // Easting DOP (0.01)
} ubx_nav_dop_t;

/**
 * @brief Структура данных принятого пакета ACK/NAK
 */
typedef struct {
    uint8_t clsID; // Класс подтвержденного сообщения
    uint8_t msgID; // ID подтвержденного сообщения
} ubx_ack_t;

/**
 * @brief Основной контекст FSM парсера UBX
 */
typedef struct {
    ubx_state_t state;                  // Текущее состояние FSM
    uint8_t     msg_class;              // Принятый класс сообщения
    uint8_t     msg_id;                 // Принятый ID сообщения
    uint16_t    payload_len;            // Длина полезной нагрузки
    uint16_t    payload_idx;            // Индекс текущего байта полезной нагрузки
    uint8_t     ck_a;                   // Расчетная контрольная сумма A
    uint8_t     ck_b;                   // Расчетная контрольная сумма B
    uint8_t     rx_ck_a;                // Принятая контрольная сумма A
    uint8_t     rx_ck_b;                // Принятая контрольная сумма B
    uint8_t     payload[UBX_MAX_PAYLOAD_SIZE]; // Буфер полезной нагрузки

    // Распакованные актуальные данные
    ubx_nav_pvt_t last_pvt;             // Последние принятые данные PVT
    ubx_nav_dop_t last_dop;             // Последние принятые данные DOP
    ubx_ack_t     last_ack;             // Последний принятый ACK/NAK
    bool          pvt_valid;            // Флаг наличии свежего пакета PVT
} ubx_parser_t;

/**
 * @brief Инициализация контекста парсера UBX.
 * @param parser Указатель на структуру контекста.
 */
void ubx_parser_init(ubx_parser_t *parser);

/**
 * @brief Потоковая обработка одного полученного байта.
 * @param parser Указатель на контекст парсера.
 * @param byte Принятый байт с UART.
 * @return Результат обработки (статус принятия полного пакета или продолжение чтения).
 */
ubx_parse_result_t ubx_parser_process_byte(ubx_parser_t *parser, uint8_t byte);

/**
 * @brief Расчет контрольной суммы Алгоритма Флетчера (Fletcher-8) по стандарту UBX.
 * @param data Указатель на данные.
 * @param len Длина данных в байтах.
 * @param ck_a Выходной указатель на CK_A.
 * @param ck_b Выходной указатель на CK_B.
 */
void ubx_calc_checksum(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b);

/**
 * @brief Формирование бинарного пакета UBX-CFG-PRT для настройки скорости UART1.
 * @param buffer Выходной буфер (размер не менее 28 байт).
 * @param baudrate Желаемая скорость UART (например, 115200).
 * @return Итоговый размер сформированного пакета в байтах.
 */
size_t ubx_build_cfg_prt_baud(uint8_t *buffer, uint32_t baudrate);

/**
 * @brief Формирование пакета UBX-CFG-NAV5 для настройки масок PDOP и PACC.
 * @param buffer Выходной буфер (размер не менее 44 байт).
 * @param pdop_mask Маска PDOP (0 - не менять, иначе значение * 10).
 * @param pacc_mask Маска Position Accuracy в метрах (0 - не менять).
 * @return Размер сформированного пакета в байтах.
 */
size_t ubx_build_cfg_nav5(uint8_t *buffer, uint16_t pdop_mask, uint16_t pacc_mask);

/**
 * @brief Формирование бинарного запроса версии UBX-MON-VER (Poll).
 * @param buffer Выходной буфер (размер не менее 8 байт).
 * @return Размер пакета (8 байт).
 */
size_t ubx_build_mon_ver_poll(uint8_t *buffer);

/**
 * @brief Формирование бинарного пакета настройки навигационной частоты UBX-CFG-RATE.
 * @param buffer Выходной буфер (размер не менее 14 байт).
 * @param meas_rate_ms Период измерений в мс (например, 1000 мс = 1 Гц).
 * @return Размер пакета в байтах.
 */
size_t ubx_build_cfg_rate(uint8_t *buffer, uint16_t meas_rate_ms);

/**
 * @brief Формирование пакета UBX-CFG-MSG для включения/выключения UBX-сообщения на текущем порту.
 *
 * Используется упрощённая 3-байтовая форма CFG-MSG (SET), которая устанавливает частоту
 * вывода для порта, через который отправлена команда (UART1 в нашем случае).
 *
 * @param buffer  Выходной буфер (размер не менее 11 байт).
 * @param msg_cls Класс целевого сообщения (например, UBX_CLASS_NAV = 0x01).
 * @param msg_id  ID целевого сообщения (например, UBX_NAV_PVT = 0x07).
 * @param rate    Частота вывода: 0 — выключить, 1 — каждую навигационную эпоху.
 * @return Размер сформированного пакета в байтах (11).
 */
size_t ubx_build_cfg_msg(uint8_t *buffer, uint8_t msg_cls, uint8_t msg_id, uint8_t rate);

#ifdef __cplusplus
}
#endif

#endif // UBX_PARSER_H
