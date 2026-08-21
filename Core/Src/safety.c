#include "safety.h"

#include "main.h"
#include "hw_config.h"

volatile uint32_t safety_fault_latched = SAFETY_FAULT_NONE;

void safety_outputs_off(void)
{
#ifdef STM32F446
    HAL_GPIO_WritePin(ENABLE_PIN, GPIO_PIN_RESET);
    __HAL_TIM_MOE_DISABLE(&TIM_PWM);
    __HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_U, 0U);
    __HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_V, 0U);
    __HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_W, 0U);
#else
    /* PA11 is the independent DRV8323 enable. Drive it low first so shutdown
     * does not depend on SPI, the control loop, or timer state. */
    gpio_bit_reset(ENABLE_PIN);
    timer_primary_output_config(TIM_PWM, DISABLE);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_U, 0U);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_V, 0U);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_W, 0U);
#endif
}

void safety_force_outputs_off(uint32_t reason)
{
    safety_fault_latched |= reason;
    safety_outputs_off();
}

uint32_t safety_get_faults(void)
{
    return safety_fault_latched;
}
