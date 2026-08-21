#include "motor_gate.h"

MotorGateResult motor_gate_check(uint32_t safety_faults,
                                 uint16_t drv_fault,
                                 uint8_t nfault_high,
                                 uint8_t encoder_valid,
                                 uint8_t adc_valid)
{
    if (safety_faults != 0U) {
        return MOTOR_GATE_SAFETY_FAULT;
    }
    if (drv_fault != 0U) {
        return MOTOR_GATE_DRV_FAULT;
    }
    if (nfault_high == 0U) {
        return MOTOR_GATE_NFAULT_LOW;
    }
    if (encoder_valid == 0U) {
        return MOTOR_GATE_ENCODER_INVALID;
    }
    if (adc_valid == 0U) {
        return MOTOR_GATE_ADC_INVALID;
    }
    return MOTOR_GATE_OK;
}

uint8_t motor_gate_precharge_complete(uint32_t started_ms,
                                      uint32_t now_ms,
                                      uint32_t required_ms)
{
    return (uint8_t)((uint32_t)(now_ms - started_ms) >= required_ms);
}
