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

#ifndef STM32F446
void diagnostics_handle_can(const can_receive_message_struct *message);
void diagnostics_uart_report(void);
#endif

#endif /* INC_DIAGNOSTICS_H_ */
