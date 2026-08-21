#ifndef INC_INJECT_H_
#define INC_INJECT_H_

#include "can.h"
#include <stdint.h>

#ifdef BRINGUP_INJECT

/* Boot-time gate-driver configuration and zero-current offset capture. The
 * power stage stays off: PA11 low, all PWM channels at the all-low compare. */
void inject_init(void);

/* Called from the 30 kHz timer ISR. Maintains the safe state whenever no
 * injection is active and enforces deadline/fault/current limits during one. */
void inject_timer_tick(void);

/* CAN RX dispatcher for the BRINGUP_INJECT profile. Handles the gated INJECT
 * and STOP opcodes itself and forwards all read-only opcodes to the existing
 * diagnostic handler. */
void inject_handle_can(const can_receive_message_struct *message);

/* Runs the bounded DRV wake/configuration transaction outside all ISRs. */
void inject_service(void);

/* True only during the bounded, PWM-disabled DRV fault-read window. */
int inject_drv_wake_window_active(void);

/* One-line per-second UART status appended after the standard diagnostic
 * lines. */
void inject_uart_report(void);

/* Read-only snapshot pages 20..48 for the inject profile. */
uint32_t inject_snapshot(uint8_t page, uint8_t *status);

#endif /* BRINGUP_INJECT */

#endif /* INC_INJECT_H_ */
