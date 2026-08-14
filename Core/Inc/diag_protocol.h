#ifndef INC_DIAG_PROTOCOL_H_
#define INC_DIAG_PROTOCOL_H_

#include <stdint.h>

#define DIAG_PROTOCOL_MAJOR 1U
#define DIAG_NODE_ID 1U
#define DIAG_CAN_REQUEST_ID (0x700U | DIAG_NODE_ID)
#define DIAG_CAN_RESPONSE_ID (0x780U | DIAG_NODE_ID)

typedef enum {
    DIAG_OPCODE_PING = 0x00U,
    DIAG_OPCODE_GET_INFO = 0x01U,
    DIAG_OPCODE_GET_SNAPSHOT = 0x02U,
    DIAG_OPCODE_GET_COUNTER = 0x03U
} DiagOpcode;

typedef enum {
    DIAG_STATUS_OK = 0U,
    DIAG_STATUS_BAD_PAGE = 1U,
    DIAG_STATUS_BUSY = 2U,
    DIAG_STATUS_UNAVAILABLE = 3U,
    DIAG_STATUS_UNSUPPORTED = 4U
} DiagStatus;

typedef struct {
    uint8_t opcode;
    uint8_t sequence;
    uint8_t page;
} DiagRequest;

uint8_t diag_crc8_atm(const uint8_t *data, uint32_t length);
int diag_protocol_parse(const uint8_t data[8], DiagRequest *request);
void diag_protocol_response(const DiagRequest *request, uint8_t status,
                            uint32_t payload, uint8_t data[8]);

#endif /* INC_DIAG_PROTOCOL_H_ */
