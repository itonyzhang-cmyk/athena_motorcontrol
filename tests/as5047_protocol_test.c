#include <assert.h>
#include <stdint.h>

#include "as5047_protocol.h"

void as5047_protocol_tests(void)
{
    assert(as5047_make_read_command(0x0000U) == 0xC000U);
    assert(as5047_make_read_command(0x0001U) == 0x4001U);
    assert(as5047_make_read_command(0x3FFCU) == 0xFFFCU);
    assert(as5047_make_read_command(0x3FFDU) == 0x7FFDU);
    assert(as5047_make_read_command(0x3FFEU) == 0x7FFEU);
    assert(as5047_make_read_command(0x3FFFU) == 0xFFFFU);

    assert(as5047_response_status(0x0000U) == 0);
    assert(as5047_response_status(0x8001U) == 0);
    assert(as5047_response_status(0x0001U) == -1);
    assert(as5047_response_status(0xC000U) == -2);

    assert(as5047_wrapped_delta(3U, 16380U) == 7);
    assert(as5047_wrapped_delta(16380U, 3U) == -7);
    assert(as5047_wrapped_delta(1000U, 999U) == 1);
}
