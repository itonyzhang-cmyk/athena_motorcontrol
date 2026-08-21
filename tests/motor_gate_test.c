#include <assert.h>
#include <stdio.h>

#include "motor_gate.h"

void config_store_tests(void);
void normal_can_protocol_tests(void);

static void expect(MotorGateResult expected, uint32_t faults, uint16_t drv_fault,
                   uint8_t nfault, uint8_t encoder, uint8_t adc)
{
    assert(motor_gate_check(faults, drv_fault, nfault, encoder, adc) == expected);
}

int main(void)
{
    config_store_tests();
    normal_can_protocol_tests();
    expect(MOTOR_GATE_OK, 0U, 0U, 1U, 1U, 1U);
    expect(MOTOR_GATE_SAFETY_FAULT, 1U, 0U, 1U, 1U, 1U);
    expect(MOTOR_GATE_DRV_FAULT, 0U, 1U, 1U, 1U, 1U);
    expect(MOTOR_GATE_NFAULT_LOW, 0U, 0U, 0U, 1U, 1U);
    expect(MOTOR_GATE_ENCODER_INVALID, 0U, 0U, 1U, 0U, 1U);
    expect(MOTOR_GATE_ADC_INVALID, 0U, 0U, 1U, 1U, 0U);
    expect(MOTOR_GATE_SAFETY_FAULT, 0x20U, 1U, 0U, 0U, 0U);
    assert(motor_gate_precharge_complete(100U, 109U, 10U) == 0U);
    assert(motor_gate_precharge_complete(100U, 110U, 10U) == 1U);
    assert(motor_gate_precharge_complete(UINT32_MAX - 4U, 5U, 10U) == 1U);
    puts("motor gate tests: PASS");
    return 0;
}
