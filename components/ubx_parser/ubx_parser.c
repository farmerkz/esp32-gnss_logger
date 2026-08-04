/**
 * @file ubx_parser.c
 * @brief Реализация потокового парсера UBX и генераторов конфигурационных сообщений.
 */

#include "ubx_parser.h"
#include <string.h>

void ubx_calc_checksum(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
    uint8_t a = 0;
    uint8_t b = 0;
    for (size_t i = 0; i < len; i++) {
        a = a + data[i];
        b = b + a;
    }
    *ck_a = a;
    *ck_b = b;
}

void ubx_parser_init(ubx_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    memset(parser, 0, sizeof(ubx_parser_t));
    parser->state = UBX_STATE_IDLE;
}

ubx_parse_result_t ubx_parser_process_byte(ubx_parser_t *parser, uint8_t byte)
{
    if (parser == NULL) {
        return UBX_PARSE_ERROR;
    }

    switch (parser->state) {
    case UBX_STATE_IDLE:
        // Ожидаем первый синхробайт 0xB5
        if (byte == UBX_SYNC_CHAR1) {
            parser->state = UBX_STATE_SYNC2;
        }
        break;

    case UBX_STATE_SYNC2:
        // Ожидаем второй синхробайт 0x62
        if (byte == UBX_SYNC_CHAR2) {
            parser->state = UBX_STATE_CLASS;
            parser->ck_a = 0;
            parser->ck_b = 0;
        } else if (byte == UBX_SYNC_CHAR1) {
            // Повторный первый байт
            parser->state = UBX_STATE_SYNC2;
        } else {
            parser->state = UBX_STATE_IDLE;
        }
        break;

    case UBX_STATE_CLASS:
        // Считываем класс сообщения и обновляем контрольную сумму
        parser->msg_class = byte;
        parser->ck_a += byte;
        parser->ck_b += parser->ck_a;
        parser->state = UBX_STATE_ID;
        break;

    case UBX_STATE_ID:
        // Считываем ID сообщения и обновляем контрольную сумму
        parser->msg_id = byte;
        parser->ck_a += byte;
        parser->ck_b += parser->ck_a;
        parser->state = UBX_STATE_LEN_LSB;
        break;

    case UBX_STATE_LEN_LSB:
        // Младший байт длины payload
        parser->payload_len = byte;
        parser->ck_a += byte;
        parser->ck_b += parser->ck_a;
        parser->state = UBX_STATE_LEN_MSB;
        break;

    case UBX_STATE_LEN_MSB:
        // Старший байт длины payload
        parser->payload_len |= ((uint16_t)byte << 8);
        parser->ck_a += byte;
        parser->ck_b += parser->ck_a;
        parser->payload_idx = 0;

        // Если длина превышает допустимый размер буфера — сбрасываем FSM
        if (parser->payload_len > UBX_MAX_PAYLOAD_SIZE) {
            parser->state = UBX_STATE_IDLE;
            return UBX_PARSE_ERROR;
        }

        if (parser->payload_len == 0) {
            parser->state = UBX_STATE_CK_A;
        } else {
            parser->state = UBX_STATE_PAYLOAD;
        }
        break;

    case UBX_STATE_PAYLOAD:
        // Заполняем буфер полезной нагрузки
        parser->payload[parser->payload_idx++] = byte;
        parser->ck_a += byte;
        parser->ck_b += parser->ck_a;

        if (parser->payload_idx >= parser->payload_len) {
            parser->state = UBX_STATE_CK_A;
        }
        break;

    case UBX_STATE_CK_A:
        // Считываем принятую контрольную сумму A
        parser->rx_ck_a = byte;
        parser->state = UBX_STATE_CK_B;
        break;

    case UBX_STATE_CK_B:
        // Считываем принятую контрольную сумму B и проверяем её
        parser->rx_ck_b = byte;
        parser->state = UBX_STATE_IDLE; // Сбрасываем FSM для следующего пакета

        if (parser->ck_a != parser->rx_ck_a || parser->ck_b != parser->rx_ck_b) {
            return UBX_PARSE_ERROR; // Несовпадение контрольной суммы
        }

        // Пакет валиден. Разбираем известные типы сообщений.
        if (parser->msg_class == UBX_CLASS_NAV && parser->msg_id == UBX_NAV_PVT) {
            if (parser->payload_len >= sizeof(ubx_nav_pvt_t)) {
                memcpy(&parser->last_pvt, parser->payload, sizeof(ubx_nav_pvt_t));
                parser->pvt_valid = true;
                return UBX_PARSE_GOT_PVT;
            }
        } else if (parser->msg_class == UBX_CLASS_NAV && parser->msg_id == UBX_NAV_DOP) {
            if (parser->payload_len >= sizeof(ubx_nav_dop_t)) {
                memcpy(&parser->last_dop, parser->payload, sizeof(ubx_nav_dop_t));
                return UBX_PARSE_GOT_DOP;
            }
        } else if (parser->msg_class == UBX_CLASS_ACK) {
            if (parser->payload_len >= 2) {
                parser->last_ack.clsID = parser->payload[0];
                parser->last_ack.msgID = parser->payload[1];
                return (parser->msg_id == UBX_ACK_ACK) ? UBX_PARSE_GOT_ACK : UBX_PARSE_GOT_NAK;
            }
        } else if (parser->msg_class == UBX_CLASS_MON && parser->msg_id == UBX_MON_VER) {
            return UBX_PARSE_GOT_MON_VER;
        }

        return UBX_PARSE_GOT_OTHER;

    default:
        parser->state = UBX_STATE_IDLE;
        break;
    }

    return UBX_PARSE_BUSY;
}

size_t ubx_build_cfg_prt_baud(uint8_t *buffer, uint32_t baudrate)
{
    if (buffer == NULL) {
        return 0;
    }

    // Структурная полезная нагрузка UBX-CFG-PRT для UART1 (20 байт)
    uint8_t payload[20] = {0};

    payload[0] = 1;    // Port ID = 1 (UART1)
    payload[1] = 0;    // Reserved
    payload[2] = 0;    // txReady (Disabled)
    payload[3] = 0;

    // Mode: 8N1 (0x08D0 = 8 bits, no parity, 1 stop bit)
    uint32_t mode = 0x08D0;
    payload[4] = mode & 0xFF;
    payload[5] = (mode >> 8) & 0xFF;
    payload[6] = (mode >> 16) & 0xFF;
    payload[7] = (mode >> 24) & 0xFF;

    // BaudRate (little endian 32-bit int)
    payload[8]  = baudrate & 0xFF;
    payload[9]  = (baudrate >> 8) & 0xFF;
    payload[10] = (baudrate >> 16) & 0xFF;
    payload[11] = (baudrate >> 24) & 0xFF;

    // inProtoMask = 0x0007 (UBX + NMEA + RTCM)
    payload[12] = 0x07;
    payload[13] = 0x00;

    // outProtoMask = 0x0001 (UBX only)
    payload[14] = 0x01;
    payload[15] = 0x00;

    // flags = 0
    payload[16] = 0;
    payload[17] = 0;
    payload[18] = 0;
    payload[19] = 0;

    // Формируем полный пакет: SYNC(2) + CLASS(1) + ID(1) + LEN(2) + PAYLOAD(20) + CK(2) = 28 байт
    buffer[0] = UBX_SYNC_CHAR1;
    buffer[1] = UBX_SYNC_CHAR2;
    buffer[2] = UBX_CLASS_CFG;
    buffer[3] = UBX_CFG_PRT;
    buffer[4] = 20; // Length LSB
    buffer[5] = 0;  // Length MSB

    memcpy(&buffer[6], payload, 20);

    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buffer[2], 4 + 20, &ck_a, &ck_b);

    buffer[26] = ck_a;
    buffer[27] = ck_b;

    return 28;
}

size_t ubx_build_cfg_nav5(uint8_t *buffer, uint16_t pdop_mask, uint16_t pacc_mask)
{
    if (buffer == NULL) {
        return 0;
    }

    // Полезная нагрузка UBX-CFG-NAV5 (36 байт)
    uint8_t payload[36] = {0};

    uint16_t mask = 0;
    if (pdop_mask > 0) {
        mask |= (1 << 2); // pDop bit mask
    }
    if (pacc_mask > 0) {
        mask |= (1 << 3); // pAcc bit mask
    }

    payload[0] = mask & 0xFF;        // applyMask LSB
    payload[1] = (mask >> 8) & 0xFF; // applyMask MSB
    payload[2] = 3;                  // dynModel = Pedestrian (3)
    payload[3] = 3;                  // fixMode = Auto 2D/3D

    // pDop mask (*10)
    uint16_t pdop_val = pdop_mask * 10;
    payload[14] = pdop_val & 0xFF;
    payload[15] = (pdop_val >> 8) & 0xFF;

    // pAcc mask (в метрах)
    payload[18] = pacc_mask & 0xFF;
    payload[19] = (pacc_mask >> 8) & 0xFF;

    buffer[0] = UBX_SYNC_CHAR1;
    buffer[1] = UBX_SYNC_CHAR2;
    buffer[2] = UBX_CLASS_CFG;
    buffer[3] = UBX_CFG_NAV5;
    buffer[4] = 36;
    buffer[5] = 0;

    memcpy(&buffer[6], payload, 36);

    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buffer[2], 4 + 36, &ck_a, &ck_b);

    buffer[42] = ck_a;
    buffer[43] = ck_b;

    return 44;
}

size_t ubx_build_mon_ver_poll(uint8_t *buffer)
{
    if (buffer == NULL) {
        return 0;
    }

    buffer[0] = UBX_SYNC_CHAR1;
    buffer[1] = UBX_SYNC_CHAR2;
    buffer[2] = UBX_CLASS_MON;
    buffer[3] = UBX_MON_VER;
    buffer[4] = 0; // Length = 0
    buffer[5] = 0;

    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buffer[2], 4, &ck_a, &ck_b);

    buffer[6] = ck_a;
    buffer[7] = ck_b;

    return 8;
}

size_t ubx_build_cfg_rate(uint8_t *buffer, uint16_t meas_rate_ms)
{
    if (buffer == NULL) {
        return 0;
    }

    uint8_t payload[6] = {0};
    payload[0] = meas_rate_ms & 0xFF;
    payload[1] = (meas_rate_ms >> 8) & 0xFF;
    payload[2] = 1; // navRate = 1 cycles
    payload[3] = 0;
    payload[4] = 0; // timeRef = 0 (UTC)
    payload[5] = 0;

    buffer[0] = UBX_SYNC_CHAR1;
    buffer[1] = UBX_SYNC_CHAR2;
    buffer[2] = UBX_CLASS_CFG;
    buffer[3] = UBX_CFG_RATE;
    buffer[4] = 6;
    buffer[5] = 0;

    memcpy(&buffer[6], payload, 6);

    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buffer[2], 4 + 6, &ck_a, &ck_b);

    buffer[12] = ck_a;
    buffer[13] = ck_b;

    return 14;
}

size_t ubx_build_cfg_msg(uint8_t *buffer, uint8_t msg_cls, uint8_t msg_id, uint8_t rate)
{
    if (buffer == NULL) {
        return 0;
    }

    // Упрощённая 3-байтовая форма UBX-CFG-MSG (SET):
    // Устанавливает частоту вывода сообщения на порту, через который отправлена команда.
    // payload = [msgClass, msgID, rate]
    uint8_t payload[3] = { msg_cls, msg_id, rate };

    buffer[0] = UBX_SYNC_CHAR1;
    buffer[1] = UBX_SYNC_CHAR2;
    buffer[2] = UBX_CLASS_CFG;
    buffer[3] = UBX_CFG_MSG;
    buffer[4] = 3; // Length LSB
    buffer[5] = 0; // Length MSB

    memcpy(&buffer[6], payload, 3);

    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buffer[2], 4 + 3, &ck_a, &ck_b);

    buffer[9]  = ck_a;
    buffer[10] = ck_b;

    return 11;
}
