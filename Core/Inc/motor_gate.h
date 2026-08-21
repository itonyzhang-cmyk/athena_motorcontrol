#ifndef INC_MOTOR_GATE_H_
#define INC_MOTOR_GATE_H_

#include <stdint.h>

typedef enum {
    MOTOR_GATE_OK = 0,
    MOTOR_GATE_SAFETY_FAULT = 1,
    MOTOR_GATE_DRV_FAULT = 2,
    MOTOR_GATE_NFAULT_LOW = 3,
    MOTOR_GATE_ENCODER_INVALID = 4,
    MOTOR_GATE_ADC_INVALID = 5
} MotorGateResult;

MotorGateResult motor_gate_check(uint32_t safety_faults,
                                 uint16_t drv_fault,
                                 uint8_t nfault_high,
                                 uint8_t encoder_valid,
                                 uint8_t adc_valid);
uint8_t motor_gate_precharge_complete(uint32_t started_ms,
                                      uint32_t now_ms,
                                      uint32_t required_ms);

#endif
