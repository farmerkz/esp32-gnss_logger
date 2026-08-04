// test_ubx_parser.c
// Контрольные тесты для компонента ubx_parser
//
// Компиляция (из корня проекта):
//   gcc -o test_ubx_parser \
//       test_ubx_parser.c \
//       components/ubx_parser/ubx_parser.c \
//       -I components/ubx_parser/include \
//       -Wall -Wextra
//
// Запуск:
//   ./test_ubx_parser

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ubx_parser.h"

// ---------------------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------------------

// Подать массив байт в парсер побайтово, вернуть последний результат
static ubx_parse_result_t feed_bytes(ubx_parser_t *p, const uint8_t *data, size_t len)
{
    ubx_parse_result_t res = UBX_PARSE_BUSY;
    for (size_t i = 0; i < len; i++) {
        res = ubx_parser_process_byte(p, data[i]);
    }
    return res;
}

// Собрать полный UBX-пакет: SYNC + CLASS + ID + LEN + PAYLOAD + CK
static size_t build_ubx_packet(uint8_t *buf, uint8_t cls, uint8_t id,
                                const uint8_t *payload, uint16_t plen)
{
    buf[0] = UBX_SYNC_CHAR1;
    buf[1] = UBX_SYNC_CHAR2;
    buf[2] = cls;
    buf[3] = id;
    buf[4] = (uint8_t)(plen & 0xFF);
    buf[5] = (uint8_t)((plen >> 8) & 0xFF);
    if (plen > 0 && payload != NULL) {
        memcpy(&buf[6], payload, plen);
    }
    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buf[2], 4 + plen, &ck_a, &ck_b);
    buf[6 + plen] = ck_a;
    buf[7 + plen] = ck_b;
    return 8 + plen;
}

// ---------------------------------------------------------------------------
// TC-01: UBX-ACK-ACK — подтверждение команды CFG-PRT
// ---------------------------------------------------------------------------
static void test_tc01_ack_ack(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // payload: clsID=0x06 (CFG), msgID=0x00 (PRT)
    uint8_t payload[] = {0x06, 0x00};
    uint8_t buf[32];
    size_t len = build_ubx_packet(buf, UBX_CLASS_ACK, UBX_ACK_ACK, payload, 2);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    assert(res == UBX_PARSE_GOT_ACK);
    assert(parser.last_ack.clsID == 0x06);
    assert(parser.last_ack.msgID == 0x00);
    printf("[PASS] TC-01: ACK-ACK для CFG-PRT\n");
}

// ---------------------------------------------------------------------------
// TC-02: UBX-ACK-NAK — отклонение команды CFG-NAV5
// ---------------------------------------------------------------------------
static void test_tc02_ack_nak(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // payload: clsID=0x06 (CFG), msgID=0x24 (NAV5)
    uint8_t payload[] = {0x06, 0x24};
    uint8_t buf[32];
    size_t len = build_ubx_packet(buf, UBX_CLASS_ACK, UBX_ACK_NAK, payload, 2);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    assert(res == UBX_PARSE_GOT_NAK);
    assert(parser.last_ack.clsID == 0x06);
    assert(parser.last_ack.msgID == 0x24);
    printf("[PASS] TC-02: ACK-NAK для CFG-NAV5\n");
}

// ---------------------------------------------------------------------------
// TC-03: UBX-MON-VER — минимальный ответ на Poll-запрос
// ---------------------------------------------------------------------------
static void test_tc03_mon_ver_minimal(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // Небольшой payload (реальный — десятки байт, но парсер не проверяет длину)
    uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t buf[32];
    size_t len = build_ubx_packet(buf, UBX_CLASS_MON, UBX_MON_VER, payload, 4);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    assert(res == UBX_PARSE_GOT_MON_VER);
    printf("[PASS] TC-03: MON-VER (минимальный payload)\n");
}

// ---------------------------------------------------------------------------
// TC-04: UBX-NAV-DOP — полный пакет Dilution of Precision (18 байт)
// ---------------------------------------------------------------------------
static void test_tc04_nav_dop(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // iTOW=453600000, gDOP=120, pDOP=100, tDOP=80, vDOP=90, hDOP=60, nDOP=50, eDOP=40
    uint8_t payload[18] = {
        0x00, 0x63, 0x09, 0x1B,  // iTOW = 453 600 000 мс (0x1B096300, little-endian)
        0x78, 0x00,               // gDOP = 120 (1.20)
        0x64, 0x00,               // pDOP = 100 (1.00)
        0x50, 0x00,               // tDOP = 80  (0.80)
        0x5A, 0x00,               // vDOP = 90  (0.90)
        0x3C, 0x00,               // hDOP = 60  (0.60)
        0x32, 0x00,               // nDOP = 50  (0.50)
        0x28, 0x00                // eDOP = 40  (0.40)
    };
    uint8_t buf[64];
    size_t len = build_ubx_packet(buf, UBX_CLASS_NAV, UBX_NAV_DOP, payload, 18);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    assert(res == UBX_PARSE_GOT_DOP);
    assert(parser.last_dop.iTOW == 453600000UL);
    assert(parser.last_dop.gDOP == 120);
    assert(parser.last_dop.pDOP == 100);
    assert(parser.last_dop.tDOP == 80);
    assert(parser.last_dop.vDOP == 90);
    assert(parser.last_dop.hDOP == 60);
    assert(parser.last_dop.nDOP == 50);
    assert(parser.last_dop.eDOP == 40);
    printf("[PASS] TC-04: NAV-DOP (18 байт)\n");
}

// ---------------------------------------------------------------------------
// TC-05: UBX-NAV-PVT — 3D фикс, координаты Москвы
// ---------------------------------------------------------------------------
static void test_tc05_nav_pvt_3d_fix(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // Структура ubx_nav_pvt_t = 92 байта, little-endian
    uint8_t payload[92];
    memset(payload, 0, sizeof(payload));

    // iTOW = 453600000 (0x1B077940)
    payload[0] = 0x40; payload[1] = 0x79; payload[2] = 0x07; payload[3] = 0x1B;
    // year = 2024 (0x07E8), little-endian
    payload[4] = 0xE8; payload[5] = 0x07;
    // month=6, day=15, hour=12, min=30, sec=0
    payload[6] = 6; payload[7] = 15; payload[8] = 12; payload[9] = 30; payload[10] = 0;
    // valid = 0x07 (validDate | validTime | fullyResolved)
    payload[11] = 0x07;
    // tAcc = 30 нс
    payload[12] = 30; payload[13] = 0; payload[14] = 0; payload[15] = 0;
    // nano = 0 (bytes 16..19)
    // fixType = 3 (3D Fix)
    payload[20] = 3;
    // flags = 0x01 (gnssFixOK)
    payload[21] = 0x01;
    // flags2 = 0
    payload[22] = 0x00;
    // numSV = 12
    payload[23] = 12;
    // lon = 376173000 (37.6173 deg * 1e7) → 0x166BF1C8
    payload[24] = 0xC8; payload[25] = 0xF1; payload[26] = 0x6B; payload[27] = 0x16;
    // lat = 557558000 (55.7558 deg * 1e7) → 0x213BA8F0
    payload[28] = 0xF0; payload[29] = 0xA8; payload[30] = 0x3B; payload[31] = 0x21;
    // height = 150000 мм (150 м)
    payload[32] = 0xF0; payload[33] = 0x49; payload[34] = 0x02; payload[35] = 0x00;
    // hMSL = 148000 мм → 0x00024220
    payload[36] = 0x20; payload[37] = 0x42; payload[38] = 0x02; payload[39] = 0x00;
    // hAcc = 2500 мм
    payload[40] = 0xC4; payload[41] = 0x09; payload[42] = 0; payload[43] = 0;
    // vAcc = 3000 мм
    payload[44] = 0xB8; payload[45] = 0x0B; payload[46] = 0; payload[47] = 0;
    // остальные поля = 0

    uint8_t buf[128];
    size_t len = build_ubx_packet(buf, UBX_CLASS_NAV, UBX_NAV_PVT, payload, 92);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    assert(res == UBX_PARSE_GOT_PVT);
    assert(parser.pvt_valid == true);
    assert(parser.last_pvt.year == 2024);
    assert(parser.last_pvt.month == 6);
    assert(parser.last_pvt.day == 15);
    assert(parser.last_pvt.hour == 12);
    assert(parser.last_pvt.fixType == 3);
    assert((parser.last_pvt.flags & 0x01) == 1);
    assert(parser.last_pvt.numSV == 12);
    assert(parser.last_pvt.lon == 376173000L);
    assert(parser.last_pvt.lat == 557558000L);
    assert(parser.last_pvt.hMSL == 148000L);
    assert(parser.last_pvt.hAcc == 2500UL);
    printf("[PASS] TC-05: NAV-PVT 3D Fix (Москва)\n");
}

// ---------------------------------------------------------------------------
// TC-06: Ошибочная контрольная сумма (CK_B неверный)
// ---------------------------------------------------------------------------
static void test_tc06_bad_crc(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // Корректный ACK-ACK: B5 62 05 01 02 00 06 00 0E 37
    // Портим CK_B: 0x37 → 0x38
    uint8_t buf[] = {0xB5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x00, 0x0E, 0x38};

    ubx_parse_result_t res = feed_bytes(&parser, buf, sizeof(buf));

    assert(res == UBX_PARSE_ERROR);
    assert(parser.state == UBX_STATE_IDLE); // FSM сброшен
    printf("[PASS] TC-06: Неверный CRC → UBX_PARSE_ERROR\n");
}

// ---------------------------------------------------------------------------
// TC-07: Переполнение payload (len = 512 > UBX_MAX_PAYLOAD_SIZE=256)
// ---------------------------------------------------------------------------
static void test_tc07_payload_overflow(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // Подаём только заголовок с len=0x0200 (512)
    // LEN_LSB=0x00, LEN_MSB=0x02
    uint8_t buf[] = {0xB5, 0x62, 0x01, 0x07, 0x00, 0x02};

    ubx_parse_result_t res = feed_bytes(&parser, buf, sizeof(buf));

    assert(res == UBX_PARSE_ERROR);
    assert(parser.state == UBX_STATE_IDLE);
    printf("[PASS] TC-07: Переполнение payload → UBX_PARSE_ERROR\n");
}

// ---------------------------------------------------------------------------
// TC-08: Мусорные байты перед валидным пакетом
// ---------------------------------------------------------------------------
static void test_tc08_noise_before_packet(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    uint8_t buf[] = {
        0xFF, 0x00, 0xAA, 0x12,                              // мусор
        0xB5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x00, 0x0E, 0x37 // ACK-ACK
    };

    ubx_parse_result_t res = feed_bytes(&parser, buf, sizeof(buf));

    assert(res == UBX_PARSE_GOT_ACK);
    assert(parser.last_ack.clsID == 0x06);
    printf("[PASS] TC-08: Мусорные байты перед пакетом игнорируются\n");
}

// ---------------------------------------------------------------------------
// TC-09: Повторный 0xB5 в состоянии SYNC2
// ---------------------------------------------------------------------------
static void test_tc09_repeated_sync1(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // B5 B5 62 — второй B5 не сбрасывает FSM, остаёмся в SYNC2
    uint8_t buf[] = {
        0xB5, 0xB5,                                          // двойной sync1
        0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x00, 0x0E, 0x37 // ACK-ACK
    };

    ubx_parse_result_t res = feed_bytes(&parser, buf, sizeof(buf));

    assert(res == UBX_PARSE_GOT_ACK);
    printf("[PASS] TC-09: Повторный 0xB5 в SYNC2 обрабатывается корректно\n");
}

// ---------------------------------------------------------------------------
// TC-10: NAV-DOP с payload меньше sizeof(ubx_nav_dop_t)=18 байт
// ---------------------------------------------------------------------------
static void test_tc10_dop_short_payload(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    // Только 10 байт payload (< 18)
    uint8_t payload[10] = {0};
    uint8_t buf[32];
    size_t len = build_ubx_packet(buf, UBX_CLASS_NAV, UBX_NAV_DOP, payload, 10);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    // payload_len < sizeof(ubx_nav_dop_t) → блок DOP не выполняется
    assert(res == UBX_PARSE_GOT_OTHER);
    printf("[PASS] TC-10: NAV-DOP с коротким payload → UBX_PARSE_GOT_OTHER\n");
}

// ---------------------------------------------------------------------------
// TC-11: ACK-ACK с нулевым payload (len=0)
// ---------------------------------------------------------------------------
static void test_tc11_ack_zero_payload(void)
{
    ubx_parser_t parser;
    ubx_parser_init(&parser);

    uint8_t buf[32];
    // payload = NULL, len = 0
    size_t len = build_ubx_packet(buf, UBX_CLASS_ACK, UBX_ACK_ACK, NULL, 0);

    ubx_parse_result_t res = feed_bytes(&parser, buf, len);

    // payload_len < 2 → условие ACK не выполнено
    assert(res == UBX_PARSE_GOT_OTHER);
    printf("[PASS] TC-11: ACK с нулевым payload → UBX_PARSE_GOT_OTHER\n");
}

// ---------------------------------------------------------------------------
// TC-12: ubx_build_cfg_msg() — включение NAV-PVT rate=1
// ---------------------------------------------------------------------------
static void test_tc12_build_cfg_msg(void)
{
    uint8_t buf[16];

    // Построить CFG-MSG для включения NAV-PVT (0x01/0x07) с rate=1
    size_t len = ubx_build_cfg_msg(buf, UBX_CLASS_NAV, UBX_NAV_PVT, 1);

    // Проверяем размер пакета: SYNC(2) + CLASS(1) + ID(1) + LEN(2) + PAYLOAD(3) + CK(2) = 11
    assert(len == 11);

    // Проверяем синхробайты
    assert(buf[0] == UBX_SYNC_CHAR1);
    assert(buf[1] == UBX_SYNC_CHAR2);

    // Проверяем класс и ID
    assert(buf[2] == UBX_CLASS_CFG);
    assert(buf[3] == UBX_CFG_MSG);

    // Проверяем длину payload
    assert(buf[4] == 3);
    assert(buf[5] == 0);

    // Проверяем payload: [msg_cls=0x01, msg_id=0x07, rate=0x01]
    assert(buf[6] == UBX_CLASS_NAV);
    assert(buf[7] == UBX_NAV_PVT);
    assert(buf[8] == 1);

    // Проверяем CRC: пересчитываем вручную и сравниваем
    uint8_t ck_a = 0, ck_b = 0;
    ubx_calc_checksum(&buf[2], 4 + 3, &ck_a, &ck_b);
    assert(buf[9]  == ck_a);
    assert(buf[10] == ck_b);

    // Дополнительно: парсер должен принять пакет как GOT_OTHER (CFG-MSG не имеет парсера)
    ubx_parser_t parser;
    ubx_parser_init(&parser);
    ubx_parse_result_t res = UBX_PARSE_BUSY;
    for (size_t i = 0; i < len; i++) {
        res = ubx_parser_process_byte(&parser, buf[i]);
    }
    assert(res == UBX_PARSE_GOT_OTHER);

    printf("[PASS] TC-12: ubx_build_cfg_msg() NAV-PVT rate=1\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void)
{
    printf("=== UBX Parser Test Suite ===\n");
    printf("Тест-кейсы для компонента components/ubx_parser\n\n");

    test_tc01_ack_ack();
    test_tc02_ack_nak();
    test_tc03_mon_ver_minimal();
    test_tc04_nav_dop();
    test_tc05_nav_pvt_3d_fix();
    test_tc06_bad_crc();
    test_tc07_payload_overflow();
    test_tc08_noise_before_packet();
    test_tc09_repeated_sync1();
    test_tc10_dop_short_payload();
    test_tc11_ack_zero_payload();
    test_tc12_build_cfg_msg();

    printf("\n=== Все тесты пройдены успешно ===\n");
    return 0;
}
