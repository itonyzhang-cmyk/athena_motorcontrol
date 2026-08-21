#include "normal_can_protocol.h"

bool normal_can_frame_matches(uint32_t frame_id, uint8_t standard_frame,
                              uint8_t data_frame, uint8_t dlc,
                              int configured_id)
{
    return standard_frame != 0U && data_frame != 0U && dlc == 8U &&
           configured_id >= 0 && configured_id <= 0x7FF &&
           frame_id == (uint32_t)configured_id;
}

bool normal_can_special_command(const uint8_t data[8], uint8_t *command)
{
    for (unsigned i = 0U; i < 7U; ++i) {
        if (data[i] != 0xFFU) {
            return false;
        }
    }
    if (data[7] != 0xFCU && data[7] != 0xFDU && data[7] != 0xFEU) {
        return false;
    }
    *command = data[7];
    return true;
}

bool normal_can_diag_ping_matches(uint32_t frame_id, uint8_t standard_frame,
                                  uint8_t data_frame, uint8_t dlc,
                                  const uint8_t data[8], DiagRequest *request)
{
    if (frame_id != DIAG_CAN_REQUEST_ID || standard_frame == 0U ||
        data_frame == 0U || dlc != 8U ||
        diag_protocol_parse(data, request) != 0) {
        return false;
    }

    return request->opcode == DIAG_OPCODE_PING && request->page == 0U;
}
