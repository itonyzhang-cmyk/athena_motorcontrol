#ifndef INC_RUNTIME_WATCHDOG_H_
#define INC_RUNTIME_WATCHDOG_H_

#include <stdint.h>

/* Hardware-independent progress gate used by the GD32 FWDGT backend. */
uint8_t runtime_watchdog_should_reload(uint32_t observed_progress,
                                       uint32_t current_progress);

/* Configure the independent watchdog after interrupts are enabled.
 * Returns zero only when the hardware watchdog is armed. */
int runtime_watchdog_init(void);
void runtime_watchdog_main_heartbeat(void);
void runtime_watchdog_timer_service(void);
uint8_t runtime_watchdog_active(void);

#endif /* INC_RUNTIME_WATCHDOG_H_ */
