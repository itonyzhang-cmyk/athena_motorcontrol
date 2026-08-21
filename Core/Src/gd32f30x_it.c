/*!
    \file    gd32f30x_it.c
    \brief   interrupt service routines

    \version 2017-02-10, V1.0.0, firmware for GD32F30x
    \version 2018-10-10, V1.1.0, firmware for GD32F30x
    \version 2018-12-25, V2.0.0, firmware for GD32F30x
    \version 2020-09-30, V2.1.0, firmware for GD32F30x 
*/

/*
    Copyright (c) 2020, GigaDevice Semiconductor Inc.

    Redistribution and use in source and binary forms, with or without modification, 
are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice, this 
       list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice, 
       this list of conditions and the following disclaimer in the documentation 
       and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its contributors 
       may be used to endorse or promote products derived from this software without 
       specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT 
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR 
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY 
OF SUCH DAMAGE.
*/

#include "gd32f30x_it.h"
#include "main.h"
#include "systick.h"

#include "structs.h"
#include "usart.h"
#include "fsm.h"
#include "spi.h"
#include "gpio.h"
#include "adc.h"
#include "foc.h"
#include "can.h"
#include "position_sensor.h"
#include "hw_config.h"
#include "user_config.h"
#include "safety.h"
#include "diagnostics.h"
#include "diag_protocol.h"
#include "normal_can_protocol.h"
#ifdef BRINGUP_INJECT
#include "inject.h"
#endif


/*!
    \brief      this function handles NMI exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void NMI_Handler(void)
{
}

/*!
    \brief      this function handles HardFault exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void HardFault_Handler(void)
{
    safety_force_outputs_off(SAFETY_FAULT_HARDFAULT);
    /* if Hard Fault exception occurs, go to infinite loop */
    while (1){
    }
}

/*!
    \brief      this function handles MemManage exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void MemManage_Handler(void)
{
    safety_force_outputs_off(SAFETY_FAULT_MEMMANAGE);
    /* if Memory Manage exception occurs, go to infinite loop */
    while (1){
    }
}

/*!
    \brief      this function handles BusFault exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void BusFault_Handler(void)
{
    safety_force_outputs_off(SAFETY_FAULT_BUSFAULT);
    /* if Bus Fault exception occurs, go to infinite loop */
    while (1){
    }
}

/*!
    \brief      this function handles UsageFault exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void UsageFault_Handler(void)
{
    safety_force_outputs_off(SAFETY_FAULT_USAGEFAULT);
    /* if Usage Fault exception occurs, go to infinite loop */
    while (1){
    }
}

/*!
    \brief      this function handles SVC exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void SVC_Handler(void)
{
}

/*!
    \brief      this function handles DebugMon exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void DebugMon_Handler(void)
{
}

/*!
    \brief      this function handles PendSV exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void PendSV_Handler(void)
{
}

/*!
    \brief      this function handles SysTick exception
    \param[in]  none
    \param[out] none
    \retval     none
*/
void SysTick_Handler(void)
{
    delay_decrement();
}


void USBD_LP_CAN0_RX0_IRQHandler(void)
{
    can_message_receive(CAN0, CAN_FIFO0, &can_rx);

#ifdef BRINGUP_INJECT
    inject_handle_can(&can_rx);
    return;
#endif

#ifdef SAFE_BRINGUP
#ifdef CAN_PROBE
    /* Keep the normal diagnostic handler linked for the safety audit; the
     * probe intentionally bypasses it at runtime. */
    static void (*volatile keep_diagnostic_handler)(const can_receive_message_struct *) =
        diagnostics_handle_can;
    (void)keep_diagnostic_handler;
    /* CAN path probe only: reply to any received standard data frame without
     * applying the diagnostic parser. Gate drive remains disabled by the safe
     * profile; this isolates CAN RX interrupt and TX arbitration. */
    if (can_rx.rx_ff == CAN_FF_STANDARD && can_rx.rx_ft == CAN_FT_DATA &&
        can_rx.rx_dlen == 8U) {
        can_trasnmit_message_struct probe;
        can_struct_para_init(CAN_TX_MESSAGE_STRUCT, &probe);
        probe.tx_sfid = DIAG_CAN_RESPONSE_ID;
        probe.tx_efid = 0U;
        probe.tx_ft = CAN_FT_DATA;
        probe.tx_ff = CAN_FF_STANDARD;
        probe.tx_dlen = 8U;
        probe.tx_data[0] = can_rx.rx_data[3] | 0x80U;
        probe.tx_data[1] = can_rx.rx_data[4];
        probe.tx_data[2] = can_rx.rx_data[5];
        probe.tx_data[3] = DIAG_STATUS_OK;
        probe.tx_data[4] = 0U;
        probe.tx_data[5] = 0U;
        probe.tx_data[6] = 0U;
        probe.tx_data[7] = 0U;
        can_message_transmit(CAN0, &probe);
    }
#else
    safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
    diagnostics_handle_can(&can_rx);
#endif
    return;
#endif

    /* Preserve the CAN0 diagnostic ping used to validate the bring-up image.
     * This is deliberately narrower than ATHENA-DIAG: only a CRC-valid
     * read-only ping is answered; all driver, injection and motion opcodes
     * remain unavailable in the normal application. */
    {
        DiagRequest diagnostic_request;

        if (normal_can_diag_ping_matches(can_rx.rx_sfid,
                                         can_rx.rx_ff == CAN_FF_STANDARD,
                                         can_rx.rx_ft == CAN_FT_DATA,
                                         can_rx.rx_dlen, can_rx.rx_data,
                                         &diagnostic_request)) {
            can_trasnmit_message_struct diagnostic_response;

            can_struct_para_init(CAN_TX_MESSAGE_STRUCT, &diagnostic_response);
            diagnostic_response.tx_sfid = DIAG_CAN_RESPONSE_ID;
            diagnostic_response.tx_efid = 0U;
            diagnostic_response.tx_ft = CAN_FT_DATA;
            diagnostic_response.tx_ff = CAN_FF_STANDARD;
            diagnostic_response.tx_dlen = 8U;
            diag_protocol_response(&diagnostic_request, DIAG_STATUS_OK,
                                   0x4E485441U, diagnostic_response.tx_data);
            (void)can_message_transmit(CAN0, &diagnostic_response);
            return;
        }
    }

    /* Normal MIT application path: hardware filter is intentionally permissive
     * for GD32 compatibility; reject all non-matching frames here. */
    if (!normal_can_frame_matches(can_rx.rx_sfid,
                                  can_rx.rx_ff == CAN_FF_STANDARD,
                                  can_rx.rx_ft == CAN_FT_DATA,
                                  can_rx.rx_dlen, CAN_ID)) {
        return;
    }

#ifdef DEBUG_CAN
    debug("sid: 0x%04lx eid: 0x%08lx format: %u type: %u length: %u\r\n",
          can_rx.rx_sfid, can_rx.rx_efid, can_rx.rx_ff, can_rx.rx_ft, can_rx.rx_dlen);
    debug("data: 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x 0x%02x\r\n",
          can_rx.rx_data[0], can_rx.rx_data[1], can_rx.rx_data[2], can_rx.rx_data[3],
          can_rx.rx_data[4], can_rx.rx_data[5], can_rx.rx_data[6], can_rx.rx_data[7]);
#endif

    // Pack response
    pack_reply(&can_tx, CAN_ID, comm_encoder.angle_multiturn[0] / GR, comm_encoder.velocity / GR, controller.i_q_filt * KT * GR);
    can_message_transmit(CAN0, &can_tx);

#ifdef DEBUG_CAN
    debug("CAN TX P:%.3f V:%.3f I:%.3f\r\n",
            comm_encoder.angle_multiturn[0] / GR, comm_encoder.velocity / GR, controller.i_q_filt * KT * GR);
#endif

    /* Check for special Commands */
    uint8_t special_command;
    if (normal_can_special_command(can_rx.rx_data, &special_command)) {
        switch (special_command)
        {
        case 0xFC:
            update_fsm(&state, MOTOR_CMD);
            break;
        
        case 0xFD:
            update_fsm(&state, ESC_CMD);
            break;
        
        case 0xFE:
            update_fsm(&state, ZERO_CMD);
            break;
        
        default:
            break;
        }
    } else {
        unpack_cmd(can_rx, controller.commands); // Unpack commands
        controller.timeout = 0;                  // Reset timeout counter

#ifdef DEBUG_CAN
        debug("CAN RX P:%.3f V:%.3f KP:%.3f KD:%.3f I:%.3f\r\n",
                controller.p_des, controller.v_des, controller.kp, controller.kd, controller.t_ff);
#endif
    }
}

void TIMER0_UP_IRQHandler(void)
{
    timer_interrupt_flag_clear(TIMER0, TIMER_INT_FLAG_UP);

#if !defined(SAFE_BRINGUP) && !defined(BRINGUP_INJECT)
    /* TIMER0 updates at 30 kHz (SVPWM_PERIOD=2000 on the 120 MHz clock).
     * Use this already-running highest-priority path for the normal-image
     * heartbeat, so a heavily loaded control ISR cannot starve LED timing.
     * This only writes PC13 once per 15,000 updates and is unrelated to PWM. */
    static uint16_t heartbeat_ticks;
    static bit_status heartbeat_state = RESET;
    heartbeat_ticks++;
    if (heartbeat_ticks >= 15000U) {
        heartbeat_ticks = 0U;
        heartbeat_state = (heartbeat_state == RESET) ? SET : RESET;
        gpio_bit_write(GPIOC, GPIO_PIN_13, heartbeat_state);
    }
#endif

#ifdef BRINGUP_INJECT
    inject_timer_tick();
    static uint8_t inject_diagnostic_divider;
    if (++inject_diagnostic_divider >= 30U) {
        inject_diagnostic_divider = 0U;
        analog_sample(&controller);
        ps_sample(&comm_encoder, 0.001f);
    }
    controller.loop_count++;
    return;
#endif

#ifdef SAFE_BRINGUP
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
	/* Motor-control-rate sampling is unnecessary with the power stage hard
	 * disabled. Divide 30 kHz down to 1 kHz to leave ample ISR margin. */
	static uint8_t safe_diagnostic_divider;
	if (++safe_diagnostic_divider < 30U) {
		controller.loop_count++;
		return;
	}
	safe_diagnostic_divider = 0U;
#endif
    
	/* Sample ADCs */
	analog_sample(&controller);

	/* Sample position sensor */
#if defined(SAFE_BRINGUP) || defined(BRINGUP_INJECT)
	ps_sample(&comm_encoder, 0.001f);
#else
	ps_sample(&comm_encoder, DT);
#endif

	/* Run Finite State Machine */
#if !defined(SAFE_BRINGUP) && !defined(BRINGUP_INJECT)
	run_fsm(&state);
#endif

#ifdef DEBUG_TIMER
                            //cyberdog layout      //mbed
    controller.dtc_u = 0.6; //PA8 /TIMER0_CH0/INHA //PA10
    controller.dtc_v = 0.4; //PA9 /TIMER0_CH1/INHB //PA9
    controller.dtc_w = 0.5; //PA10/TIMER0_CH2/INHC //PA8
    set_dtc(&controller);
#endif

	/* increment loop count */
	controller.loop_count++;
    
}

void EXTI10_15_IRQHandler(void)
{
    exti_interrupt_flag_clear(EXTI_12);

    /* nFAULT is active-low. It also goes low normally when PA11 has disabled
     * the DRV, so only latch it when PA11 was actually enabling the driver.
     * The dedicated wake window below keeps PA11 high solely to capture the
     * DRV status registers with PWM/POEN disabled. */
    if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
#ifdef BRINGUP_INJECT
        if (inject_drv_wake_window_active()) {
            /* PWM/POEN are disabled; preserve ENABLE for one bounded SPI
             * fault read so the diagnostic can identify the DRV failure. */
            drv.fault = 1U;
            return;
        }
#endif
        if (gpio_output_bit_get(GPIOA, GPIO_PIN_11) != RESET) {
            safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
            drv.fault = 1U;
        }
        return;
    }

#ifdef SAFE_BRINGUP
    safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
#endif
}

void USART1_IRQHandler(void)
{
    if(RESET != usart_interrupt_flag_get(USART1, USART_INT_FLAG_RBNE)){
        /* receive data */
        char c = usart_data_receive(USART1);
#if defined(BRINGUP_INJECT)
        (void)c;
#elif defined(SAFE_BRINGUP)
        (void)c;
        safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
#else
	    update_fsm(&state, c);
#endif
    }
}
