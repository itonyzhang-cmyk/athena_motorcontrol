#ifndef INC_NORMAL_CAN_PROTOCOL_H_
#define INC_NORMAL_CAN_PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>

#include "diag_protocol.h"

bool normal_can_frame_matches(uint32_t frame_id, uint8_t standard_frame,
                              uint8_t data_frame, uint8_t dlc,
                              int configured_id);
bool normal_can_special_command(const uint8_t data[8], uint8_t *command);
/* Compatibility proof path for the previously validated CAN0 diagnostic
 * transport. Only the read-only ping request is accepted by normal firmware. */
bool normal_can_diag_ping_matches(uint32_t frame_id, uint8_t standard_frame,
                                  uint8_t data_frame, uint8_t dlc,
                                  const uint8_t data[8], DiagRequest *request);

#endif /* INC_NORMAL_CAN_PROTOCOL_H_ */
