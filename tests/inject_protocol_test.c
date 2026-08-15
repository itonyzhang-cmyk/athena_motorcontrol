#include "diag_protocol.h"

#include <stdio.h>

static int build_request(uint8_t opcode, uint8_t sequence, uint8_t page,
                         uint8_t packed, uint8_t data[8])
{
    data[0] = 0xA5U;
    data[1] = 0x5AU;
    data[2] = DIAG_PROTOCOL_MAJOR;
    data[3] = opcode;
    data[4] = sequence;
    data[5] = page;
    data[6] = packed;
    data[7] = diag_crc8_atm(data, 7U);
    return 0;
}

static int expect_ok(const uint8_t data[8], uint8_t opcode, uint8_t vector,
                     uint8_t duty_idx, uint8_t duration_idx)
{
    InjectRequest request;
    if (inject_protocol_parse(data, &request) != 0) return 1;
    if (request.opcode != opcode || request.vector != vector ||
        request.duty_idx != duty_idx || request.duration_idx != duration_idx) {
        return 1;
    }
    return 0;
}

static int expect_reject(uint8_t *data)
{
    InjectRequest request;
    return inject_protocol_parse(data, &request) == 0 ? 1 : 0;
}

int main(void)
{
    uint8_t data[8];
    InjectRequest request;

    build_request(DIAG_OPCODE_INJECT, 0x11U, 0U, 0x00U, data);
    if (expect_ok(data, DIAG_OPCODE_INJECT, 0U, 0U, 0U) != 0) return 1;

    build_request(DIAG_OPCODE_INJECT, 0x22U, 5U, 0x49U, data);
    if (expect_ok(data, DIAG_OPCODE_INJECT, 5U, 9U, 4U) != 0) return 1;

    build_request(DIAG_OPCODE_INJECT_STOP, 0x33U, 0U, 0x00U, data);
    if (expect_ok(data, DIAG_OPCODE_INJECT_STOP, 0U, 0U, 0U) != 0) return 1;

    build_request(DIAG_OPCODE_INJECT, 0x44U, 6U, 0x00U, data);
    if (expect_reject(data) != 0) return 1;

    build_request(DIAG_OPCODE_INJECT, 0x55U, 0U, 0x0AU, data);
    if (expect_reject(data) != 0) return 1;

    build_request(DIAG_OPCODE_INJECT, 0x66U, 0U, 0x50U, data);
    if (expect_reject(data) != 0) return 1;

    build_request(DIAG_OPCODE_INJECT, 0x77U, 0U, 0x00U, data);
    data[7] ^= 0xFFU;
    if (expect_reject(data) != 0) return 1;

    data[0] = 0x00U;
    if (inject_protocol_parse(data, &request) >= 0) return 1;

    puts("PASS: INJECT request golden vectors, table bounds, and CRC rejection.");
    return 0;
}
