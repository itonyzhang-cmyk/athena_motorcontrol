#ifndef INC_AS5047_PROTOCOL_H_
#define INC_AS5047_PROTOCOL_H_

#include <stdint.h>

#define AS5047_DATA_MASK 0x3FFFU
#define AS5047_ERROR_BIT 0x4000U

uint16_t as5047_make_read_command(uint16_t address);
uint16_t as5047_make_write_command(uint16_t address, uint16_t value);
int as5047_response_status(uint16_t frame);
int16_t as5047_wrapped_delta(uint16_t current, uint16_t previous);

#endif /* INC_AS5047_PROTOCOL_H_ */
