#ifndef INC_SAFETY_H_
#define INC_SAFETY_H_

#include <stdint.h>

typedef enum {
    SAFETY_FAULT_NONE = 0U,
    SAFETY_FAULT_ERROR_HANDLER = (1U << 0),
    SAFETY_FAULT_HARDFAULT = (1U << 1),
    SAFETY_FAULT_MEMMANAGE = (1U << 2),
    SAFETY_FAULT_BUSFAULT = (1U << 3),
    SAFETY_FAULT_USAGEFAULT = (1U << 4),
    SAFETY_FAULT_GATE_DRIVER = (1U << 5),
    SAFETY_FAULT_ENCODER = (1U << 6),
    SAFETY_FAULT_ADC_TIMEOUT = (1U << 7),
    SAFETY_FAULT_SPI_TIMEOUT = (1U << 8),
    SAFETY_FAULT_WATCHDOG_CONFIG = (1U << 9),
    SAFETY_FAULT_OVERCURRENT = (1U << 10),
    SAFETY_FAULT_UNDERVOLTAGE = (1U << 11),
    SAFETY_FAULT_OVERVOLTAGE = (1U << 12),
    SAFETY_FAULT_OVERTEMPERATURE = (1U << 13),
    SAFETY_FAULT_CURRENT_SENSOR = (1U << 14),
    SAFETY_FAULT_VBUS_SENSOR = (1U << 15),
    SAFETY_FAULT_TEMP_SENSOR = (1U << 16),
    SAFETY_FAULT_SAFE_BRINGUP = (1U << 31)
} SafetyFault;

extern volatile uint32_t safety_fault_latched;

void safety_force_outputs_off(uint32_t reason);
void safety_outputs_off(void);
uint32_t safety_get_faults(void);
/* Clear only faults explicitly re-armed by a new enable session. */
void safety_clear_faults(uint32_t mask);

#endif /* INC_SAFETY_H_ */
