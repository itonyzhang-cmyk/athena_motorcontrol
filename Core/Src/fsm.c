/*
 * fsm.cpp
 *
 *  Created on: Mar 5, 2020
 *      Author: Ben
 */

#include "fsm.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "user_config.h"
#include "hw_config.h"
#include "structs.h"
#include "foc.h"
#include "math_ops.h"
#include "position_sensor.h"
#include "drv8323.h"
#include "safety.h"
#include "motor_gate.h"
#include "diagnostics.h"

#ifdef SAFE_BRINGUP

void run_fsm(FSMStruct *fsmstate)
{
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);

	if (fsmstate->next_state != MENU_MODE &&
	    fsmstate->next_state != ENCODER_MODE) {
		fsmstate->next_state = MENU_MODE;
	}

	if (fsmstate->state == INIT_TEMP_MODE) {
		fsmstate->state = MENU_MODE;
		fsmstate->next_state = MENU_MODE;
		fsmstate->ready = 1U;
		enter_menu_state();
	} else if (fsmstate->next_state != fsmstate->state) {
		fsm_exit_state(fsmstate);
		fsmstate->state = fsmstate->next_state;
		fsm_enter_state(fsmstate);
	}

	if (fsmstate->state == ENCODER_MODE) {
		ps_print(&comm_encoder, controller.loop_count);
	}
}

void update_fsm(FSMStruct *fsmstate, char fsm_input)
{
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);

	if (fsm_input == ESC_CMD) {
		fsmstate->next_state = MENU_MODE;
	} else if (fsmstate->state == MENU_MODE && fsm_input == ENCODER_CMD) {
		fsmstate->next_state = ENCODER_MODE;
	}
}

void fsm_enter_state(FSMStruct *fsmstate)
{
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
	fsmstate->ready = 1U;
	if (fsmstate->state == MENU_MODE) {
		enter_menu_state();
	}
}

void fsm_exit_state(FSMStruct *fsmstate)
{
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
	fsmstate->ready = 1U;
}

void enter_menu_state(void)
{
	printf("\r\n SAFE_BRINGUP commands:\r\n");
	printf(" e   - Display encoder (read-only)\r\n");
	printf(" esc - Exit to safe menu\r\n");
	printf(" Motor, calibration, setup, zero and Flash writes are disabled.\r\n");
}

void enter_setup_state(void)
{
	printf("\r\n Setup is disabled by SAFE_BRINGUP.\r\n");
}

void process_user_input(FSMStruct *fsmstate)
{
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
	fsmstate->bytecount = 0;
	fsmstate->cmd_id = 0;
	memset(fsmstate->cmd_buff, 0, sizeof(fsmstate->cmd_buff));
}

void enter_motor_mode(void)
{
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
}

#else

static int save_preferences(void)
{
	if (!preference_writer_open(&prefs)) {
		return -1;
	}
	if (!preference_writer_flush(&prefs)) {
		preference_writer_close(&prefs);
		return -1;
	}
	preference_writer_close(&prefs);
	return preference_writer_load(&prefs) ? 0 : -1;
}

static MotorGateResult motor_gate_preflight(void)
{
#ifdef STM32F446
	const uint8_t nfault_high = (uint8_t)(HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_12) != GPIO_PIN_RESET);
#else
	const uint8_t nfault_high = (uint8_t)(gpio_input_bit_get(GPIOA, GPIO_PIN_12) != RESET);
#endif
	return motor_gate_check(safety_get_faults(), drv.fault, nfault_high,
	                        comm_encoder.valid, controller.adc_valid);
}

	 void run_fsm(FSMStruct * fsmstate){
	 /* run_fsm is run every commutation interrupt cycle */
	 /* A fresh 0xFC may arrive immediately after the previous session's
	  * shutdown.  Give the encoder and DRV one bounded control-rate window to
	  * report healthy data before attempting MOTOR_MODE. */
	 if (fsmstate->rearm_pending != 0U) {
		 /* Service the DRV enable window before checking nFAULT.  The service
		  * routine owns the bounded CLR_FLT retry; returning early here would
		  * leave every recovery attempt stuck in state=0 forever. */
		 drv_service_enable(drv);
		 const uint8_t nfault_high =
			 (uint8_t)(gpio_input_bit_get(GPIOA, GPIO_PIN_12) != RESET);
		 if (drv_enable_ready() != 0 && comm_encoder.valid != 0U && controller.adc_valid != 0U &&
			 nfault_high != 0U) {
			 /* drv.fault is a software latch from the previous session.  The
			  * explicit 0xFC recovery request permits clearing it once the
			  * physical nFAULT input has returned high; drv_service_enable() still
			  * performs the complete SPI readback gate below. */
			 drv.fault = 0U;
			 safety_clear_faults(SAFETY_FAULT_GATE_DRIVER |
				 SAFETY_FAULT_ENCODER | SAFETY_FAULT_SPI_TIMEOUT |
				 SAFETY_FAULT_ADC_TIMEOUT);
			 fsmstate->rearm_pending = 0U;
			 fsmstate->rearm_wait_cycles = 0U;
			 /* update_fsm(MOTOR_CMD) deliberately held the transition until
			  * the DRV/encoder/ADC preflight completed.  Commit it now; without
			  * restoring ready, a healthy rearm still remained stuck in MENU. */
			 fsmstate->ready = 1U;
		 } else if (fsmstate->rearm_wait_cycles++ >= 3000U) {
			 /* 100 ms at 30 kHz is enough for a transient SPI/DRV recovery;
			  * persistent faults remain fail-closed and can be diagnosed. */
			 fsmstate->rearm_pending = 0U;
			 fsmstate->rearm_wait_cycles = 0U;
			 fsmstate->next_state = MENU_MODE;
			 fsmstate->ready = 1U;
		 } else {
			 return;
		 }
	 }

	 /* state transition management */
	 if(fsmstate->next_state != fsmstate->state){
		 fsm_exit_state(fsmstate);		// safely exit the old state
		 if(fsmstate->ready){			// if the previous state is ready, enter the new state
			 fsmstate->state = fsmstate->next_state;
			 fsm_enter_state(fsmstate);
		 }
	 }

	 switch(fsmstate->state){
		 case MENU_MODE:
			 break;

		 case CALIBRATION_MODE:
			/* GD32 gate enable is non-blocking: drv_enable_gd() only starts
			 * the charge-pump/configuration window.  Service it here as well as
			 * in MOTOR_MODE; otherwise calibration runs with PWM/POEN disabled
			 * and the rotor never follows the calibration field. */
			 drv_service_enable(drv);
			 if (!drv_enable_ready()) {
				/* Do not leave EN_GATE asserted forever if DRV readback cannot
				 * complete. Return to MENU so the next session can retry. */
				if ((uint32_t)(controller.loop_count - comm_encoder_cal.gate_wait_start) >
				    (uint32_t)(200U / (DT * 1000.0f))) {
					diagnostics_debug_record(DIAG_DEBUG_EVENT_GATE_PREFLIGHT, 2U);
					drv_disable_gd(drv);
					comm_encoder_cal.failed = 1U;
					comm_encoder_cal.done_cal = 1U;
					comm_encoder_cal.done_ordering = 1U;
					fsmstate->next_state = MENU_MODE;
					fsmstate->ready = 1U;
				}
				 break;
			}
			 if(!comm_encoder_cal.done_ordering){
				 order_phases(&comm_encoder, &controller, &comm_encoder_cal, controller.loop_count);
			 }
			 else if(!comm_encoder_cal.done_cal){
				 calibrate_encoder(&comm_encoder, &controller, &comm_encoder_cal, controller.loop_count);
			 }
			else{
				 /* Exit calibration mode when done */
				 if (comm_encoder_cal.failed != 0U) {
					PPAIRS = comm_encoder_cal.saved_ppairs;
					PHASE_ORDER = comm_encoder_cal.saved_phase_order;
					E_ZERO = comm_encoder_cal.saved_ezero;
					memcpy(&ENCODER_LUT, comm_encoder_cal.saved_lut,
					       sizeof(comm_encoder_cal.saved_lut));
					comm_encoder.ppairs = PPAIRS;
					comm_encoder.e_zero = E_ZERO;
					memcpy(&comm_encoder.offset_lut, &ENCODER_LUT,
					       sizeof(comm_encoder.offset_lut));
					printf("Calibration rejected; previous configuration restored.\r\n");
					comm_encoder_cal.failed = 0U;
					update_fsm(fsmstate, ESC_CMD);
					break;
				 }
				 //for(int i = 0; i<128*PPAIRS; i++){printf("%d\r\n", error_array[i]);}
				 E_ZERO = comm_encoder_cal.ezero;
				 printf("E_ZERO: %d  %f\r\n", E_ZERO, TWO_PI_F*fmodf((comm_encoder.ppairs*(float)(-E_ZERO))/((float)ENC_CPR), 1.0f));
				 memcpy(&comm_encoder.offset_lut, comm_encoder_cal.lut_arr, sizeof(comm_encoder.offset_lut));
				 memcpy(&ENCODER_LUT, comm_encoder_cal.lut_arr, sizeof(comm_encoder_cal.lut_arr));
				 //for(int i = 0; i<128; i++){printf("%d\r\n", ENCODER_LUT[i]);}
				 if (save_preferences() != 0) {
					 printf("Configuration save rejected; previous data preserved.\r\n");
				 }
				 update_fsm(fsmstate, ESC_CMD);
			 }

			 break;

			 case MOTOR_MODE:
				 drv_service_enable(drv);
				 if (!drv_enable_ready()) {
					 /* PA11 may be high during the bounded charge-pump interval,
					  * but POEN remains disabled and no commutation is allowed. */
					 break;
				 }
				 /* If CAN has timed out, reset all commands */
				 uint8_t gate_ok = 1U;
				 if((CAN_TIMEOUT > 0 ) && (controller.timeout > CAN_TIMEOUT)){
					diagnostics_debug_record(DIAG_DEBUG_EVENT_WATCHDOG_TIMEOUT,
							(uint32_t)controller.timeout);
					/* A timeout is a power-stage stop, not merely a zero reference.
					 * Requiring a fresh MOTOR command makes recovery explicit. */
					zero_commands(&controller);
					drv_disable_gd(drv);
					fsmstate->next_state = MENU_MODE;
					/* The stop is already complete and outputs are disabled. Allow
					 * run_fsm() to commit the MENU transition so a later 0xFC can
					 * re-enter MOTOR_MODE without requiring a board reset. */
					fsmstate->ready = 1U;
					gate_ok = 0U;
				 } else {
					const MotorGateResult gate_result = motor_gate_preflight();
					if (gate_result != MOTOR_GATE_OK) {
						diagnostics_debug_record(DIAG_DEBUG_EVENT_GATE_PREFLIGHT,
								(uint32_t)gate_result);
						drv_disable_gd(drv);
						fsmstate->next_state = MENU_MODE;
						fsmstate->ready = 1U;
						gate_ok = 0U;
					}
				 }
			 /* Otherwise, commutate */
			 if (gate_ok != 0U){
				 torque_control(&controller);
				 field_weaken(&controller);
				 commutate(&controller, &comm_encoder);
			 }
			 /* Count only an active MOTOR session and saturate the counter.  A
			  * disabled watchdog (CAN_TIMEOUT=0) must not wrap an int after a
			  * long idle run, and a pending MENU transition must not keep aging
			  * the session that is already being shut down. */
			 if (fsmstate->next_state == MOTOR_MODE && CAN_TIMEOUT > 0 &&
			     controller.timeout <= CAN_TIMEOUT) {
				 controller.timeout++;
			 }
			 break;

		 case SETUP_MODE:
			 break;

		 case ENCODER_MODE:
			 ps_print(&comm_encoder, controller.loop_count);
			 break;

		 case INIT_TEMP_MODE:
			 break;
	 }

 }

 void fsm_enter_state(FSMStruct * fsmstate){
	 /* Called when entering a new state
	  * Do necessary setup   */

		switch(fsmstate->state){
			case MENU_MODE:
				//printf("Entering Main Menu\r\n");
				enter_menu_state();
				break;
			case SETUP_MODE:
				//printf("Entering Setup\r\n");
				enter_setup_state();
				break;
			case ENCODER_MODE:
				//printf("Entering Encoder Mode\r\n");
				break;
			case MOTOR_MODE:

				//printf("Entering Motor Mode\r\n");
#ifdef STM32F446
				HAL_GPIO_WritePin(LED, GPIO_PIN_SET );
#endif
					 {
						const MotorGateResult gate_result = motor_gate_preflight();
						if (gate_result != MOTOR_GATE_OK) {
							diagnostics_debug_record(DIAG_DEBUG_EVENT_GATE_PREFLIGHT,
									(uint32_t)gate_result);
						zero_commands(&controller);
						fsmstate->next_state = MENU_MODE;
						fsmstate->ready = 1U;
						return;
						}
					 }
				 /* Entering MOTOR_MODE after a timeout is a new watchdog session. */
				 controller.timeout = 0;
				 reset_foc(&controller);
				/* The 0xFC re-arm path may already have completed the bounded
				 * DRV verification before the FSM commits this transition. Do not
				 * start a second enable window here, which would clear
				 * drv_enable_verified and leave MOTOR_MODE permanently gated. */
				if (!drv_enable_ready())
					drv_enable_gd(drv);
				break;
			case CALIBRATION_MODE:
				//printf("Entering Calibration Mode\r\n");
				if (motor_gate_preflight() != MOTOR_GATE_OK) {
					fsmstate->next_state = MENU_MODE;
					fsmstate->ready = 1U;
					return;
				}
				/* zero out all calibrations before starting */

				comm_encoder_cal.done_cal = 0;
				comm_encoder_cal.done_ordering = 0;
				comm_encoder_cal.failed = 0;
				comm_encoder_cal.started = 0;
				comm_encoder_cal.gate_wait_start = controller.loop_count;
				comm_encoder_cal.saved_ppairs = PPAIRS;
				comm_encoder_cal.saved_phase_order = (uint8_t)PHASE_ORDER;
				comm_encoder_cal.saved_ezero = E_ZERO;
				memcpy(comm_encoder_cal.saved_lut, &ENCODER_LUT,
				       sizeof(comm_encoder_cal.saved_lut));
				comm_encoder.e_zero = 0;
				memset(&comm_encoder.offset_lut, 0, sizeof(comm_encoder.offset_lut));
				drv_enable_gd(drv);
				break;

		}
 }

 void fsm_exit_state(FSMStruct * fsmstate){
	 /* Called when exiting the current state
	  * Do necessary cleanup  */

		switch(fsmstate->state){
			case MENU_MODE:
				//printf("Leaving Main Menu\r\n");
				fsmstate->ready = 1;
				break;
			case SETUP_MODE:
				//printf("Leaving Setup Menu\r\n");
				fsmstate->ready = 1;
				break;
			case ENCODER_MODE:
				//printf("Leaving Encoder Mode\r\n");
				fsmstate->ready = 1;
				break;
			case MOTOR_MODE:
				/* Don't stop commutating if there are high currents or FW happening */
				//if( (fabs(controller.i_q_filt)<1.0f) && (fabs(controller.i_d_filt)<1.0f) ){
					fsmstate->ready = 1;
					drv_disable_gd(drv);
					reset_foc(&controller);
					//printf("Leaving Motor Mode\r\n");
#ifdef STM32F446
					HAL_GPIO_WritePin(LED, GPIO_PIN_RESET );
#endif
				//}
				zero_commands(&controller);		// Set commands to zero
				break;
			case CALIBRATION_MODE:
				//printf("Exiting Calibration Mode\r\n");
				drv_disable_gd(drv);
				if (!comm_encoder_cal.done_cal) {
					/* An aborted/incomplete calibration must not leave a mixed
					 * phase-order/pole-pair/offset state in RAM. */
					PPAIRS = comm_encoder_cal.saved_ppairs;
					PHASE_ORDER = comm_encoder_cal.saved_phase_order;
					E_ZERO = comm_encoder_cal.saved_ezero;
					memcpy(&ENCODER_LUT, comm_encoder_cal.saved_lut,
					       sizeof(comm_encoder_cal.saved_lut));
					memcpy(comm_encoder.offset_lut, comm_encoder_cal.saved_lut,
					       sizeof(comm_encoder_cal.saved_lut));
				}
				//free(error_array);
				//free(lut_array);

				fsmstate->ready = 1;
				break;
		}

 }

 void update_fsm(FSMStruct * fsmstate, char fsm_input){
	 /*update_fsm is only run when new state-change information is received
	  * on serial terminal input or CAN input
	  */
	if(fsm_input == ESC_CMD){	// escape to exit to rest mode
		fsmstate->next_state = MENU_MODE;
		fsmstate->ready = 0;
		return;
	}
	/* A CAN control session may need to recover after the watchdog has
	 * returned the FSM to MENU_MODE.  Treat a fresh MOTOR_CMD as an explicit
	 * re-arm request in every state, including the short window where a
	 * timeout has scheduled a pending MENU transition. */
	if (fsm_input == MOTOR_CMD) {
		fsmstate->next_state = MOTOR_MODE;
		if (fsmstate->state != MOTOR_MODE) {
			fsmstate->ready = 0;
			fsmstate->rearm_pending = 1U;
			fsmstate->rearm_wait_cycles = 0U;
		}
		return;
	}
	switch(fsmstate->state){
		case MENU_MODE:
			switch (fsm_input){
				case CAL_CMD:
					fsmstate->next_state = CALIBRATION_MODE;
					fsmstate->ready = 0;
					break;
				case ENCODER_CMD:
					fsmstate->next_state = ENCODER_MODE;
					fsmstate->ready = 0;
					break;
				case SETUP_CMD:
					fsmstate->next_state = SETUP_MODE;
					fsmstate->ready = 0;
					break;
				case ZERO_CMD:
					comm_encoder.m_zero = 0;
					ps_sample(&comm_encoder, DT);
					int zero_count = comm_encoder.count;
					M_ZERO = zero_count;
					if (save_preferences() != 0) {
						printf("Zero save rejected; previous data preserved.\r\n");
					}
					printf("\n\r  Saved new zero position:  %d\n\r\n\r", M_ZERO);
					break;
				}
			break;
		case SETUP_MODE:
			if(fsm_input == ENTER_CMD){
				process_user_input(fsmstate);
				break;
			}
			if(fsmstate->bytecount == 0){fsmstate->cmd_id = fsm_input;}
			else{
				fsmstate->cmd_buff[fsmstate->bytecount-1] = fsm_input;
				//fsmstate->bytecount = fsmstate->bytecount%(sizeof(fsmstate->cmd_buff)/sizeof(fsmstate->cmd_buff[0])); // reset when buffer is full
			}
			fsmstate->bytecount++;
			/* If enter is typed, process user input */

			break;

		case ENCODER_MODE:
			break;
		case MOTOR_MODE:
			break;
	}
	//printf("FSM State: %d  %d\r\n", fsmstate.state, fsmstate.state_change);
 }


 void enter_menu_state(void){
	    //drv.disable_gd();
	    //reset_foc(&controller);
	    //gpio.enable->write(0);
	    printf("\r\n");
	    printf(" Commands:\r\n");
	    printf(" m   - Motor Mode\r\n");
	    printf(" c   - Calibrate Encoder\r\n");
	    printf(" s   - Setup\r\n");
	    printf(" e   - Display Encoder\r\n");
	    printf(" z   - Set Zero Position\r\n");
	    printf(" esc - Exit to Menu\r\n");

	    //gpio.led->write(0);
 }

 void enter_setup_state(void){
	    printf("\r\n Configuration Options \n\r");
	    printf(" %-4s %-31s %-5s %-6s %-2s\r\n", "prefix", "parameter", "min", "max", "current value");
	    printf("\r\n Motor:\r\n");
	    printf(" %-4s %-31s %-5s %-6s %.3f\n\r", "g", "Gear Ratio", "0", "-", GR);
	    printf(" %-4s %-31s %-5s %-6s %.5f\n\r", "k", "Torque Constant (N-m/A)", "0", "-", KT);
	    printf("\r\n Control:\r\n");
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "b", "Current Bandwidth (Hz)", "100", "2000", I_BW);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "l", "Current Limit (A)", "0.0", "60.0", I_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "p", "Max Position Setpoint (rad)", "-", "-", P_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "v", "Max Velocity Setpoint (rad)/s", "-", "-", V_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "x", "Max Position Gain (N-m/rad)", "0.0", "1000.0", KP_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "d", "Max Velocity Gain (N-m/rad/s)", "0.0", "5.0", KD_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "f", "FW Current Limit (A)", "0.0", "33.0", I_FW_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "h", "Temp Cutoff (C) (0 = none)", "0", "150", TEMP_MAX);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "c", "Continuous Current (A)", "0.0", "40.0", I_MAX_CONT);
	    printf(" %-4s %-31s %-5s %-6s %.1f\n\r", "a", "Calibration Current (A)", "0.0", "20.0", I_CAL);
	    printf("\r\n CAN:\r\n");
	    printf(" %-4s %-31s %-5s %-6s %-5i\n\r", "i", "CAN ID", "0", "127", CAN_ID);
	    printf(" %-4s %-31s %-5s %-6s %-5i\n\r", "m", "CAN TX ID", "0", "127", CAN_MASTER);
	    printf(" %-4s %-31s %-5s %-6s %d\n\r", "t", "CAN Timeout (cycles)(0 = none)", "0", "100000", CAN_TIMEOUT);
	    printf(" \n\r To change a value, type 'prefix''value''ENTER'\n\r e.g. 'b1000''ENTER'\r\n ");
	    printf("VALUES NOT ACTIVE UNTIL POWER CYCLE! \n\r\n\r");
 }

 void process_user_input(FSMStruct * fsmstate){
	 /* Collects user input from serial (maybe eventually CAN) and updates settings */

	 switch (fsmstate->cmd_id){
		 case 'b':
			 I_BW = fmaxf(fminf(atof(fsmstate->cmd_buff), 2000.0f), 100.0f);
			 printf("I_BW set to %f\r\n", I_BW);
			 break;
		 case 'i':
			 CAN_ID = atoi(fsmstate->cmd_buff);
			 printf("CAN_ID set to %d\r\n", CAN_ID);
			 break;
		 case 'm':
			 CAN_MASTER = atoi(fsmstate->cmd_buff);
			 printf("CAN_TX_ID set to %d\r\n", CAN_MASTER);
			 break;
		 case 'l':
			 I_MAX = fmaxf(fminf(atof(fsmstate->cmd_buff), 60.0f), 0.0f);
			 printf("I_MAX set to %f\r\n", I_MAX);
			 break;
		 case 'f':
			 I_FW_MAX = fmaxf(fminf(atof(fsmstate->cmd_buff), 33.0f), 0.0f);
			 printf("I_FW_MAX set to %f\r\n", I_FW_MAX);
			 break;
		 case 't':
			 CAN_TIMEOUT = atoi(fsmstate->cmd_buff);
			 printf("CAN_TIMEOUT set to %d\r\n", CAN_TIMEOUT);
			 break;
		 case 'h':
			 TEMP_MAX = fmaxf(fminf(atof(fsmstate->cmd_buff), 150.0f), 0.0f);
			 printf("TEMP_MAX set to %f\r\n", TEMP_MAX);
			 break;
		 case 'c':
			 I_MAX_CONT = fmaxf(fminf(atof(fsmstate->cmd_buff), 40.0f), 0.0f);
			 printf("I_MAX_CONT set to %f\r\n", I_MAX_CONT);
			 break;
		 case 'a':
			 I_CAL = fmaxf(fminf(atof(fsmstate->cmd_buff), 20.0f), 0.0f);
			 printf("I_CAL set to %f\r\n", I_CAL);
			 break;
		 case 'g':
			 GR = fmaxf(atof(fsmstate->cmd_buff), .001f);	// Limit prevents divide by zero if user tries to enter zero
			 printf("GR set to %f\r\n", GR);
			 break;
		 case 'k':
			 KT = fmaxf(atof(fsmstate->cmd_buff), 0.0001f);	// Limit prevents divide by zero.  Seems like a reasonable LB?
			 printf("KT set to %f\r\n", KT);
			 break;
		 case 'x':
			 KP_MAX = fmaxf(atof(fsmstate->cmd_buff), 0.0f);
			 printf("KP_MAX set to %f\r\n", KP_MAX);
			 break;
		 case 'd':
			 KD_MAX = fmaxf(atof(fsmstate->cmd_buff), 0.0f);
			 printf("KD_MAX set to %f\r\n", KD_MAX);
			 break;
		 case 'p':
			 P_MAX = fmaxf(atof(fsmstate->cmd_buff), 0.0f);
			 P_MIN = -P_MAX;
			 printf("P_MAX set to %f\r\n", P_MAX);
			 break;
		 case 'v':
			 V_MAX = fmaxf(atof(fsmstate->cmd_buff), 0.0f);
			 V_MIN = -V_MAX;
			 printf("V_MAX set to %f\r\n", V_MAX);
			 break;
		 default:
			 printf("\r\n '%c' Not a valid command prefix\r\n\r\n", fsmstate->cmd_id);
			 break;

		 }

	 /* Write new settings to flash */

	 if (save_preferences() != 0) {
		 printf("Configuration save rejected; previous data preserved.\r\n");
	 }

	 enter_setup_state();

	 fsmstate->bytecount = 0;
	 fsmstate->cmd_id = 0;
	 memset(&fsmstate->cmd_buff, 0, sizeof(fsmstate->cmd_buff));
 }

 void enter_motor_mode(void){

 }

#endif /* SAFE_BRINGUP */
