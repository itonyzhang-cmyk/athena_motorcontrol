#include "diag_protocol.h"

uint8_t diag_crc8_atm(const uint8_t *data, uint32_t length)
{
    uint8_t crc = 0U;

    for (uint32_t i = 0U; i < length; i++) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x80U) != 0U ? (uint8_t)((crc << 1) ^ 0x07U)
                                      : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

int diag_protocol_parse(const uint8_t data[8], DiagRequest *request)
{
    if (data[0] != 0xA5U || data[1] != 0x5AU ||
        data[2] != DIAG_PROTOCOL_MAJOR || data[6] != 0U) {
        return -1;
    }
    if (diag_crc8_atm(data, 7U) != data[7]) {
        return -2;
    }

    request->opcode = data[3];
    request->sequence = data[4];
    request->page = data[5];
    return 0;
}

void diag_protocol_response(const DiagRequest *request, uint8_t status,
                            uint32_t payload, uint8_t data[8])
{
    data[0] = request->opcode | 0x80U;
    data[1] = request->sequence;
    data[2] = request->page;
    data[3] = status;
    data[4] = (uint8_t)payload;
    data[5] = (uint8_t)(payload >> 8);
    data[6] = (uint8_t)(payload >> 16);
    data[7] = (uint8_t)(payload >> 24);
}
