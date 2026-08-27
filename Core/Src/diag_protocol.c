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
        data[2] != DIAG_PROTOCOL_MAJOR ||
        (data[6] != 0U && data[3] != DIAG_OPCODE_CONTROL)) {
        return -1;
    }
    if (diag_crc8_atm(data, 7U) != data[7]) {
        return -2;
    }

    request->opcode = data[3];
    request->sequence = data[4];
    request->page = data[5];
    request->argument = data[6];
    return 0;
}

#ifdef BRINGUP_INJECT
int inject_protocol_parse(const uint8_t data[8], InjectRequest *request)
{
    if (data[0] != 0xA5U || data[1] != 0x5AU ||
        data[2] != DIAG_PROTOCOL_MAJOR) {
        return -1;
    }
    if (diag_crc8_atm(data, 7U) != data[7]) {
        return -2;
    }

    if (data[3] == DIAG_OPCODE_INJECT) {
        const uint8_t vector = data[5];
        const uint8_t duty_idx = data[6] & 0x0FU;
        const uint8_t duration_idx = data[6] >> 4;
        if (vector >= DIAG_INJECT_VECTOR_COUNT ||
            duty_idx >= DIAG_INJECT_DUTY_TABLE_SIZE ||
            duration_idx >= DIAG_INJECT_DURATION_TABLE_SIZE) {
            return -3;
        }
        request->opcode = DIAG_OPCODE_INJECT;
        request->sequence = data[4];
        request->vector = vector;
        request->duty_idx = duty_idx;
        request->duration_idx = duration_idx;
        return 0;
    }

    if (data[3] == DIAG_OPCODE_INJECT_STOP) {
        if (data[5] != 0U || data[6] != 0U) {
            return -3;
        }
        request->opcode = DIAG_OPCODE_INJECT_STOP;
        request->sequence = data[4];
        request->vector = 0U;
        request->duty_idx = 0U;
        request->duration_idx = 0U;
        return 0;
    }

    return -4;
}
#endif

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
