#ifndef INC_DIAGNOSTICS_H_
#define INC_DIAGNOSTICS_H_

#include <stdint.h>
#include "main.h"

typedef struct {
    volatile uint32_t rx_total;
    volatile uint32_t rx_valid;
    volatile uint32_t rx_bad_format;
    volatile uint32_t rx_bad_protocol;
    volatile uint32_t rx_bad_opcode_page;
    volatile uint32_t rx_rate_drop;
    volatile uint32_t tx_submit;
    volatile uint32_t tx_no_mailbox;
} DiagnosticCounters;

extern DiagnosticCounters diagnostic_counters;

#define DIAGNOSTIC_DEBUG_LOG_CAPACITY 32U

/* RAM debug event identifiers. These are deliberately sparse and stable so
 * host-side log decoders can remain compatible across normal builds. */
#define DIAG_DEBUG_EVENT_MIT_RX             1U
#define DIAG_DEBUG_EVENT_ENABLE             2U
#define DIAG_DEBUG_EVENT_DISABLE            3U
#define DIAG_DEBUG_EVENT_ZERO               4U
#define DIAG_DEBUG_EVENT_WATCHDOG_TIMEOUT   5U
#define DIAG_DEBUG_EVENT_GATE_PREFLIGHT     6U
#define DIAG_DEBUG_EVENT_FSM                7U
#define DIAG_DEBUG_EVENT_DRV_FAULT          8U
#define DIAG_DEBUG_EVENT_DEBUG_CONTROL      9U
#define DIAG_DEBUG_EVENT_CALIBRATION_FAIL  10U

typedef struct {
    uint32_t timestamp_ms;
    uint8_t event;
    uint32_t payload;
} DiagnosticDebugEntry;

void diagnostics_debug_set(uint8_t enabled);
void diagnostics_debug_clear(void);
uint8_t diagnostics_debug_enabled(void);
uint32_t diagnostics_debug_status(void);
int diagnostics_debug_read(uint8_t index, uint8_t field, uint32_t *payload);
void diagnostics_debug_record(uint8_t event, uint32_t payload);

#ifndef STM32F446
void diagnostics_handle_can(const can_receive_message_struct *message);
void diagnostics_drv_wake_service(void);
uint8_t diagnostics_drv_wake_window_active(void);
void diagnostics_uart_report(void);
#endif

#endif /* INC_DIAGNOSTICS_H_ */
