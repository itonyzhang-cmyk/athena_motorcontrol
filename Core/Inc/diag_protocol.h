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
    DIAG_OPCODE_GET_COUNTER = 0x03U,
    DIAG_OPCODE_INJECT = 0x04U,
    DIAG_OPCODE_INJECT_STOP = 0x05U,
    DIAG_OPCODE_DRV_WAKE = 0x06U,
    /* Structured replacement for the legacy UART menu commands. */
    DIAG_OPCODE_CONTROL = 0x07U
} DiagOpcode;

#define DIAG_INJECT_VECTOR_COUNT 6U
#define DIAG_INJECT_DUTY_TABLE_SIZE 10U
#define DIAG_INJECT_DURATION_TABLE_SIZE 5U

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
    uint8_t argument;
} DiagRequest;

typedef struct {
    uint8_t opcode;
    uint8_t sequence;
    uint8_t vector;
    uint8_t duty_idx;
    uint8_t duration_idx;
} InjectRequest;

uint8_t diag_crc8_atm(const uint8_t *data, uint32_t length);
int diag_protocol_parse(const uint8_t data[8], DiagRequest *request);
#ifdef BRINGUP_INJECT
int inject_protocol_parse(const uint8_t data[8], InjectRequest *request);
#endif
void diag_protocol_response(const DiagRequest *request, uint8_t status,
                            uint32_t payload, uint8_t data[8]);

#endif /* INC_DIAG_PROTOCOL_H_ */
