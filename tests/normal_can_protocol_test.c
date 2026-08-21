#include <assert.h>
#include <stdio.h>

#include "normal_can_protocol.h"

static uint8_t crc8(const uint8_t data[7])
{
    return diag_crc8_atm(data, 7U);
}

void normal_can_protocol_tests(void)
{
    uint8_t command = 0U;
    uint8_t motor_command[8] = {
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFCU
    };
    uint8_t neutral_command[8] = {
        0x7FU, 0xFFU, 0x7FU, 0xF0U, 0x00U, 0x00U, 0x07U, 0xFFU
    };
    uint8_t diagnostic_ping[8] = {0xA5U, 0x5AU, DIAG_PROTOCOL_MAJOR,
                                  DIAG_OPCODE_PING, 0x42U, 0U, 0U, 0U};
    DiagRequest diagnostic_request;

    assert(normal_can_frame_matches(1U, 1U, 1U, 8U, 1));
    assert(!normal_can_frame_matches(2U, 1U, 1U, 8U, 1));
    assert(!normal_can_frame_matches(1U, 0U, 1U, 8U, 1));
    assert(!normal_can_frame_matches(1U, 1U, 0U, 8U, 1));
    assert(!normal_can_frame_matches(1U, 1U, 1U, 7U, 1));
    assert(!normal_can_special_command(neutral_command, &command));
    assert(normal_can_special_command(motor_command, &command));
    assert(command == 0xFCU);
    motor_command[7] = 0xFDU;
    assert(normal_can_special_command(motor_command, &command));
    motor_command[7] = 0xFEU;
    assert(normal_can_special_command(motor_command, &command));
    motor_command[6] = 0U;
    assert(!normal_can_special_command(motor_command, &command));

    diagnostic_ping[7] = crc8(diagnostic_ping);
    assert(normal_can_diag_ping_matches(DIAG_CAN_REQUEST_ID, 1U, 1U, 8U,
                                        diagnostic_ping, &diagnostic_request));
    assert(diagnostic_request.sequence == 0x42U);
    assert(!normal_can_diag_ping_matches(DIAG_CAN_REQUEST_ID, 1U, 1U, 7U,
                                         diagnostic_ping, &diagnostic_request));
    diagnostic_ping[3] = DIAG_OPCODE_GET_INFO;
    diagnostic_ping[7] = crc8(diagnostic_ping);
    assert(!normal_can_diag_ping_matches(DIAG_CAN_REQUEST_ID, 1U, 1U, 8U,
                                         diagnostic_ping, &diagnostic_request));

    puts("normal CAN protocol tests: PASS");
}
