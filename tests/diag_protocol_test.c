#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "diag_protocol.h"

void as5047_protocol_tests(void);

static void check_vector(const uint8_t vector[8], uint8_t opcode,
                         uint8_t sequence, uint8_t page)
{
    DiagRequest request;
    assert(diag_crc8_atm(vector, 7U) == vector[7]);
    assert(diag_protocol_parse(vector, &request) == 0);
    assert(request.opcode == opcode);
    assert(request.sequence == sequence);
    assert(request.page == page);
}

int main(void)
{
    const uint8_t ping[8] = {0xA5, 0x5A, 0x01, 0x00, 0x01, 0x00, 0x00, 0x7D};
    const uint8_t info[8] = {0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0x00, 0xD6};
    const uint8_t snap[8] = {0xA5, 0x5A, 0x01, 0x02, 0x03, 0x03, 0x00, 0xB8};
    const uint8_t counter[8] = {0xA5, 0x5A, 0x01, 0x03, 0x04, 0x00, 0x00, 0x87};
    uint8_t mutated[8];
    uint8_t response[8];
    DiagRequest request = {DIAG_OPCODE_GET_SNAPSHOT, 0x42U, 0x09U};

    as5047_protocol_tests();
    check_vector(ping, DIAG_OPCODE_PING, 1U, 0U);
    check_vector(info, DIAG_OPCODE_GET_INFO, 2U, 0U);
    check_vector(snap, DIAG_OPCODE_GET_SNAPSHOT, 3U, 3U);
    check_vector(counter, DIAG_OPCODE_GET_COUNTER, 4U, 0U);

    for (uint32_t byte = 0U; byte < 8U; byte++) {
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            memcpy(mutated, ping, sizeof(mutated));
            mutated[byte] ^= (uint8_t)(1U << bit);
            assert(diag_protocol_parse(mutated, &request) != 0);
        }
    }

    request.opcode = DIAG_OPCODE_GET_SNAPSHOT;
    request.sequence = 0x42U;
    request.page = 0x09U;
    diag_protocol_response(&request, DIAG_STATUS_OK, 0x78563412U, response);
    assert(response[0] == 0x82U && response[1] == 0x42U &&
           response[2] == 0x09U && response[3] == 0x00U);
    assert(response[4] == 0x12U && response[5] == 0x34U &&
           response[6] == 0x56U && response[7] == 0x78U);

    puts("diag protocol tests: PASS");
    return 0;
}
