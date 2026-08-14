#include "as5047_protocol.h"

uint16_t as5047_make_read_command(uint16_t address)
{
    uint16_t command = 0x4000U | (address & AS5047_DATA_MASK);
    if (__builtin_parity((unsigned int)command) != 0) {
        command |= 0x8000U;
    }
    return command;
}

int as5047_response_status(uint16_t frame)
{
    if (__builtin_parity((unsigned int)frame) != 0) {
        return -1;
    }
    if ((frame & AS5047_ERROR_BIT) != 0U) {
        return -2;
    }
    return 0;
}

int16_t as5047_wrapped_delta(uint16_t current, uint16_t previous)
{
    int32_t delta = (int32_t)(current & AS5047_DATA_MASK) -
                    (int32_t)(previous & AS5047_DATA_MASK);
    if (delta > 8192) {
        delta -= 16384;
    } else if (delta < -8192) {
        delta += 16384;
    }
    return (int16_t)delta;
}
