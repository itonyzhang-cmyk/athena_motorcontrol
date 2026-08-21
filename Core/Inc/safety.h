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
    SAFETY_FAULT_SAFE_BRINGUP = (1U << 31)
} SafetyFault;

extern volatile uint32_t safety_fault_latched;

void safety_force_outputs_off(uint32_t reason);
void safety_outputs_off(void);
uint32_t safety_get_faults(void);

#endif /* INC_SAFETY_H_ */
