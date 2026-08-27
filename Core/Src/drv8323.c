/*
 * drv8323.c
 *
 *  Created on: Aug 1, 2020
 *      Author: ben
 */


#include "drv8323.h"
#include <stdio.h>
#include "structs.h"
#include "hw_config.h"
#include "user_config.h"
#include "safety.h"
#include "systick.h"
#include "motor_gate.h"
#include "foc.h"

#define DRV_ENABLE_SETTLE_MS 10U
#define DRV_DCR_CONFIG_VALUE 0x00A1U

#ifndef STM32F446
static volatile uint8_t drv_enable_pending;
static volatile uint8_t drv_enable_verified;
static volatile uint8_t drv_fault_clear_attempted;
static volatile uint8_t drv_pwm_settle_pending;
static uint32_t drv_enable_started_ms;
static int drv_verify_configuration(DRVStruct *drv);
static uint32_t drv_capture_fsr_pair(DRVStruct *drv);
#endif

static volatile uint8_t drv_init_window;
static volatile uint32_t drv_init_reason_value;
static volatile uint32_t drv_init_pre_fsr_value;
static volatile uint32_t drv_init_final_fsr_value;
static volatile uint32_t drv_init_dcr_csacr_value;
static volatile uint32_t drv_init_ocpcr_value;
static volatile uint16_t drv_init_spi_rx_value[6];
static volatile uint32_t drv_enable_evidence_value[15];
static volatile uint32_t drv_runtime_fault_evidence_value[14];
static volatile uint8_t drv_enable_window;
static volatile uint32_t drv_enable_grace_until_ms;

int drv_init_window_active(void) { return drv_init_window != 0U; }
void drv_init_record_nfault_edge(void)
{
	if (drv_init_window != 0U)
		drv_init_reason_value |= DRV_INIT_REASON_NFAULT_EDGE;
}
uint32_t drv_init_reason(void) { return drv_init_reason_value; }
uint32_t drv_init_pre_fsr(void) { return drv_init_pre_fsr_value; }
uint32_t drv_init_final_fsr(void) { return drv_init_final_fsr_value; }
uint32_t drv_init_readback_dcr_csacr(void) { return drv_init_dcr_csacr_value; }
uint32_t drv_init_readback_ocpcr(void) { return drv_init_ocpcr_value; }
uint32_t drv_init_spi_rx(uint8_t index)
{
	return index < 6U ? drv_init_spi_rx_value[index] : 0U;
}
uint32_t drv_enable_evidence(uint8_t page)
{
	return page < 15U ? drv_enable_evidence_value[page] : 0U;
}
uint32_t drv_runtime_fault_evidence(uint8_t page)
{
	return page < 14U ? drv_runtime_fault_evidence_value[page] : 0U;
}
int drv_enable_window_active(void) { return drv_enable_window != 0U; }
void drv_enable_record_nfault_edge(void) { drv_enable_evidence_value[14] = 1U; }

void drv_capture_runtime_fault(DRVStruct *drv)
{
#ifndef STM32F446
	/* This is called from the short EXTI fault path while EN_GATE is still
	 * asserted.  Preserve the pair before safety shutdown drops the DRV bank. */
	drv_runtime_fault_evidence_value[0] = systick_uptime_ms();
	drv_runtime_fault_evidence_value[2] =
		(uint32_t)(uint16_t)controller.adc_b_raw |
		((uint32_t)(uint16_t)controller.adc_c_raw << 16);
	drv_runtime_fault_evidence_value[3] =
		(uint32_t)(uint16_t)controller.adc_b_offset |
		((uint32_t)(uint16_t)controller.adc_c_offset << 16);
	drv_runtime_fault_evidence_value[4] = (uint32_t)(uint16_t)controller.adc_vbus_raw;
	drv_runtime_fault_evidence_value[5] = GPIO_ISTAT(GPIOA);
	drv_runtime_fault_evidence_value[6] = GPIO_OCTL(GPIOA);
	/* Retain the quantities actually used by the current loop before the
	 * shutdown path resets PWM/output state. These are intentionally direct
	 * controller values, not KT/GR-derived torque estimates. */
	drv_runtime_fault_evidence_value[7] = (uint32_t)(int32_t)(controller.i_q_des * 1000.0f);
	drv_runtime_fault_evidence_value[8] = (uint32_t)(int32_t)(controller.i_q * 1000.0f);
	drv_runtime_fault_evidence_value[9] = (uint32_t)(int32_t)(controller.i_d * 1000.0f);
	drv_runtime_fault_evidence_value[10] = (uint32_t)(int32_t)(controller.i_q_filt * 1000.0f);
	drv_runtime_fault_evidence_value[11] = (uint32_t)(int32_t)(controller.v_bus_filt * 1000.0f);
	drv_runtime_fault_evidence_value[12] =
		(uint32_t)(uint16_t)(controller.dtc_u * 10000.0f) |
		((uint32_t)(uint16_t)(controller.dtc_v * 10000.0f) << 16);
	drv_runtime_fault_evidence_value[13] =
		(uint32_t)(uint16_t)(controller.dtc_w * 10000.0f);
	if (drv != NULL) {
		drv_enable_evidence_value[1] = drv_capture_fsr_pair(drv);
		drv_runtime_fault_evidence_value[1] = drv_enable_evidence_value[1];
	}
#else
	(void)drv;
#endif
}

int drv_spi_transfer(DRVStruct * drv, uint16_t val, uint16_t *rx_word)
{
#ifdef STM32F446
	HAL_StatusTypeDef status;
	drv->spi_tx_word = val;
	HAL_GPIO_WritePin(DRV_CS, GPIO_PIN_RESET ); 	// CS low
	status = HAL_SPI_TransmitReceive(&DRV_SPI, (uint8_t*)drv->spi_tx_buff, (uint8_t *)drv->spi_rx_buff, 1, 100);
	while( DRV_SPI.State == HAL_SPI_STATE_BUSY );  					// wait for transmission complete
	HAL_GPIO_WritePin(DRV_CS, GPIO_PIN_SET ); 	// CS high
	*rx_word = drv->spi_rx_word;
	return status == HAL_OK ? SPI_TRANSFER_OK : SPI_TRANSFER_BUSY_TIMEOUT;
#else
	int status;
	drv->spi_tx_word = val;
	drv->spi_rx_word = 0xFFFFU;

	SPI_SET_NSS_LOW(DRV_CS);
	spi_chip_select_delay();

	status = spi_transmit_receive(DRV_SPI, &drv->spi_tx_word, &drv->spi_rx_word);
	spi_chip_select_delay();

	SPI_SET_NSS_HIGH(DRV_CS);
	/* DRV8323 requires nSCS high for at least 400 ns between 16-bit frames. */
	spi_chip_select_delay();

	*rx_word = drv->spi_rx_word;
	return status;
#endif
}

uint16_t drv_spi_write(DRVStruct * drv, uint16_t val){
	uint16_t rx_word = 0xFFFFU;
	(void)drv_spi_transfer(drv, val, &rx_word);
	return rx_word;
}
uint16_t drv_read_FSR1(DRVStruct drv){
	return drv_spi_write(&drv, (1 << 15) | (FSR1 << 11));
}

uint16_t drv_read_FSR2(DRVStruct drv){
	return drv_spi_write(&drv, (1 << 15) | (FSR2 << 11));
}

uint16_t drv_read_register(DRVStruct drv, int reg){
	return drv_spi_write(&drv, (1<<15)|(reg<<11));
}
void drv_write_register(DRVStruct drv, int reg, int val){
	drv_spi_write(&drv, (reg<<11)|val);
}
void drv_write_DCR(DRVStruct drv, int DIS_CPUV, int DIS_GDF, int OTW_REP, int PWM_MODE, int PWM_COM, int PWM_DIR, int COAST, int BRAKE, int CLR_FLT){
	uint16_t val = (DCR<<11) | (DIS_CPUV<<9) | (DIS_GDF<<8) | (OTW_REP<<7) | (PWM_MODE<<5) | (PWM_COM<<4) | (PWM_DIR<<3) | (COAST<<2) | (BRAKE<<1) | CLR_FLT;
	drv_spi_write(&drv, val);
}
void drv_write_HSR(DRVStruct drv, int LOCK, int IDRIVEP_HS, int IDRIVEN_HS){
	uint16_t val = (HSR<<11) | (LOCK<<8) | (IDRIVEP_HS<<4) | IDRIVEN_HS;
	drv_spi_write(&drv, val);
}
void drv_write_LSR(DRVStruct drv, int CBC, int TDRIVE, int IDRIVEP_LS, int IDRIVEN_LS){
	uint16_t val = (LSR<<11) | (CBC<<10) | (TDRIVE<<8) | (IDRIVEP_LS<<4) | IDRIVEN_LS;
	drv_spi_write(&drv, val);
}
void drv_write_OCPCR(DRVStruct drv, int TRETRY, int DEAD_TIME, int OCP_MODE, int OCP_DEG, int VDS_LVL){
	uint16_t val = (OCPCR<<11) | (TRETRY<<10) | (DEAD_TIME<<8) | (OCP_MODE<<6) | (OCP_DEG<<4) | VDS_LVL;
	drv_spi_write(&drv, val);
}
void drv_write_CSACR(DRVStruct drv, int CSA_FET, int VREF_DIV, int LS_REF, int CSA_GAIN, int DIS_SEN, int CSA_CAL_A, int CSA_CAL_B, int CSA_CAL_C, int SEN_LVL){
	uint16_t val = (CSACR<<11) | (CSA_FET<<10) | (VREF_DIV<<9) | (LS_REF<<8) | (CSA_GAIN<<6) | (DIS_SEN<<5) | (CSA_CAL_A<<4) | (CSA_CAL_B<<3) | (CSA_CAL_C<<2) | SEN_LVL;
	drv_spi_write(&drv, val);
}
void drv_enable_gd(DRVStruct drv){
#ifdef SAFE_BRINGUP
	(void)drv;
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
#else
	/* Arm a non-blocking power-stage transition. This function is called from
	 * TIMER0_UP_IRQHandler, so waiting on SysTick here would deadlock. */
#ifndef STM32F446
	(void)drv;
	 timer_primary_output_config(TIM_PWM, DISABLE);
	 gpio_bit_set(ENABLE_PIN);
		 for (unsigned i = 0U; i < 15U; ++i)
			 drv_enable_evidence_value[i] = 0U;
	drv_enable_window = 1U;
	drv_enable_grace_until_ms = 0U;
	 drv_enable_started_ms = systick_uptime_ms();
	drv_enable_pending = 1U;
	drv_enable_verified = 0U;
	drv_fault_clear_attempted = 0U;
	drv_pwm_settle_pending = 0U;
#else
	uint16_t val = (drv_read_register(drv, DCR)) & (~(0x1<<2));
	drv_write_register(drv, DCR, val);
	__HAL_TIM_MOE_ENABLE(&TIM_PWM);
#endif
#endif
}

void drv_service_enable(DRVStruct drv)
{
#if defined(SAFE_BRINGUP) || defined(BRINGUP_INJECT) || defined(STM32F446)
	(void)drv;
#else
	/* Consume the short post-enable grace period even after the pending
	 * transaction has completed; this must run before the pending guard. */
	if (drv_enable_verified != 0U && drv_enable_window != 0U) {
		if ((int32_t)(systick_uptime_ms() - drv_enable_grace_until_ms) >= 0) {
			if (drv_pwm_settle_pending != 0U) {
				/* Finalise the short PWM-on observation interval. A low nFAULT
				 * here is a real runtime fault; an earlier edge was retained only
				 * as startup evidence while all three outputs were neutral. */
				drv_enable_window = 0U;
				if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
					drv_enable_evidence_value[0] = 0U;
					drv_enable_evidence_value[1] = drv_capture_fsr_pair(&drv);
					safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
					drv.fault = 1U;
					drv_disable_gd(drv);
				}
				return;
			}

#ifdef DRV_GATE_ONLY_DIAGNOSTIC
			/* Keep PWM/POEN off after verified EN_GATE. This isolates an
			 * EN_GATE/charge-pump/nFAULT issue from a fault caused by PWM. */
			return;
#else
			/* Only expose PWM after the DRV charge-pump/nFAULT settling
			 * interval.  Enabling POEN in the same cycle as the final SPI
			 * readback can turn a harmless startup edge into a latched gate
			 * fault before the first MIT command arrives.
			 *
			 * drv_disable_gd() deliberately clears all three channel-enable
			 * bits, not just POEN, so every stop reaches a real electrical idle
			 * state.  Re-arm must therefore restore the channels before POEN.
			 * Otherwise the driver can pass SPI/nFAULT verification while the
			 * TIMER has no phase outputs, producing a healthy-looking but
			 * torque-free MIT session.  reset_foc() must occur here, before
			 * CH/POEN, rather than later in fsm_enter_state(): comparator values
			 * from an interrupted prior session are not a safe first PWM vector. */
			reset_foc(&controller);
			timer_channel_output_state_config(TIM_PWM, TIM_CH_U, TIMER_CCX_ENABLE);
			timer_channel_output_state_config(TIM_PWM, TIM_CH_V, TIMER_CCX_ENABLE);
			timer_channel_output_state_config(TIM_PWM, TIM_CH_W, TIMER_CCX_ENABLE);
			timer_primary_output_config(TIM_PWM, ENABLE);
			/* Keep EXTI in the bounded evidence window for the first PWM edges.
			 * The next service pass samples nFAULT after this transient is over. */
			drv_pwm_settle_pending = 1U;
			drv_enable_grace_until_ms = systick_uptime_ms() + 5U;
#endif
		}
		return;
	}
	if (drv_enable_pending == 0U ||
	    !motor_gate_precharge_complete(drv_enable_started_ms,
	                                   systick_uptime_ms(),
	                                   DRV_ENABLE_SETTLE_MS)) {
		return;
	}
	/* EN_GATE can reset the DRV register bank while low. Reapply the complete
	 * runtime configuration after every enable, with POEN still disabled. */
	if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		if (drv_fault_clear_attempted == 0U) {
			/* Clear one latched DRV fault while the bounded enable window is
			 * still active; otherwise recovery would wait forever on nFAULT. */
			drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);
			drv_fault_clear_attempted = 1U;
			return;
		}
		/* nFAULT can assert before the first configuration read. Keep EN_GATE
		 * high long enough to capture the DRV fault registers; the ISR has
		 * already suppressed the immediate shutdown while this window is safe. */
		drv_enable_evidence_value[0] = 0U;
		drv_enable_evidence_value[1] = drv_capture_fsr_pair(&drv);
		drv_enable_evidence_value[7] = GPIO_CTL1(GPIOA);
		drv_enable_evidence_value[8] = GPIO_OCTL(GPIOA);
		drv_enable_evidence_value[9] = GPIO_ISTAT(GPIOA);
		drv_enable_evidence_value[10] = GPIO_CTL1(GPIOB);
		drv_enable_evidence_value[11] = GPIO_OCTL(GPIOB);
		drv_enable_evidence_value[12] = GPIO_ISTAT(GPIOB);
		drv_enable_evidence_value[13] = SPI_STAT(SPI1);
		drv_enable_window = 0U;
		safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
		drv.fault = 1U;
		drv_enable_pending = 0U;
		return;
	}
	drv_enable_pending = 0U;
	drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);
	drv_write_register(drv, CSACR,
		I_MAX <= 40.0f ? DRV_DIAG_CSACR_VALUE_40A : DRV_DIAG_CSACR_VALUE_60A);
	drv_write_register(drv, OCPCR, DRV_DIAG_OCPCR_VALUE);
	{
		const int verify_failed = drv_verify_configuration(&drv);
		/* Preserve the readback captured while PA11 was still high. The failure
		 * path below disables the DRV, after which reads correctly return 0xFFFF. */
		drv_enable_evidence_value[0] = verify_failed == 0 ? 1U : 0U;
		drv_enable_evidence_value[1] = drv_init_final_fsr();
		drv_enable_evidence_value[2] = drv_init_readback_dcr_csacr();
		drv_enable_evidence_value[3] = drv_init_readback_ocpcr();
		for (unsigned i = 0U; i < 3U; ++i)
            drv_enable_evidence_value[4U + i] = drv_init_spi_rx((uint8_t)(i + 1U));
#ifndef STM32F446
        /* Capture the electrical/software state before the failure path lowers
         * EN_GATE. These values distinguish a DRV-side high-Z response from a
         * local GPIO/SPI peripheral state without requiring a scope. */
        drv_enable_evidence_value[7] = GPIO_CTL1(GPIOA);
        drv_enable_evidence_value[8] = GPIO_OCTL(GPIOA);
        drv_enable_evidence_value[9] = GPIO_ISTAT(GPIOA);
        drv_enable_evidence_value[10] = GPIO_CTL1(GPIOB);
        drv_enable_evidence_value[11] = GPIO_OCTL(GPIOB);
        drv_enable_evidence_value[12] = GPIO_ISTAT(GPIOB);
        drv_enable_evidence_value[13] = SPI_STAT(SPI1);
#endif
		if (verify_failed != 0 ||
	    gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		drv_enable_window = 0U;
		safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
		drv.fault = 1U;
		/* A failed readback must remove EN_GATE as well as disabling PWM. */
		drv_disable_gd(drv);
		return;
		}
	}
	drv_enable_verified = 1U;
	drv_enable_grace_until_ms = systick_uptime_ms() + 20U;
	drv_enable_window = 1U;
#endif
}

int drv_enable_ready(void)
{
#ifdef STM32F446
	return 1;
#else
	/* Verification completes before the DRV8323 nFAULT line has finished
	 * settling.  Do not let the FSM run its gate preflight in that interval:
	 * a transient-low nFAULT would immediately tear down an otherwise valid
	 * enable session.  drv_service_enable() owns the bounded grace window and
	 * clears it after the deadline. */
	return drv_enable_verified != 0U && drv_enable_window == 0U;
#endif
}

void drv_disable_gd(DRVStruct drv){
	/* Stop switching and remove the independent gate-enable immediately. This
	 * path is used by timeout, mode exit and startup idle; no disabled-DRV SPI
	 * access is attempted. */
#ifndef STM32F446
	(void)drv;
	/* MOE/POEN alone is not sufficient on GD32: the channel enable bits
	 * remain set and make the board appear unsafe to the diagnostic reader.
	 * Clear all three channel outputs before dropping EN_GATE so every stop
	 * and re-arm boundary reaches the same electrical idle state. */
	timer_channel_output_state_config(TIM_PWM, TIM_CH_U, TIMER_CCX_DISABLE);
	timer_channel_output_state_config(TIM_PWM, TIM_CH_V, TIMER_CCX_DISABLE);
	timer_channel_output_state_config(TIM_PWM, TIM_CH_W, TIMER_CCX_DISABLE);
	timer_primary_output_config(TIM_PWM, DISABLE);
	gpio_bit_reset(ENABLE_PIN);
	drv_enable_pending = 0U;
	drv_enable_verified = 0U;
	drv_enable_window = 0U;
	drv_pwm_settle_pending = 0U;
#else
	__HAL_TIM_MOE_DISABLE(&TIM_PWM);
	HAL_GPIO_WritePin(ENABLE_PIN, GPIO_PIN_RESET);
	uint16_t val = (drv_read_register(drv, DCR)) | (0x1<<2);
	drv_write_register(drv, DCR, val);
#endif
}
void drv_calibrate(DRVStruct drv){
	uint16_t val = (0x1<<4) + (0x1<<3) + (0x1<<2);
	drv_write_register(drv, CSACR, val);
}
void drv_print_faults(DRVStruct drv, uint32_t loop_count){
    uint16_t val1 = drv_read_FSR1(drv);
    uint16_t val2 = drv_read_FSR2(drv);

	if ((val1 | val2) > 0) {
		printf("loop_count: %lu\r\n", loop_count);
	}

    if(val1 & (1<<10)){printf("\n\rFAULT\n\r");}

    if(val1 & (1<<9)){printf("VDS_OCP\n\r");}
    if(val1 & (1<<8)){printf("GDF\n\r");}
    if(val1 & (1<<7)){printf("UVLO\n\r");}
    if(val1 & (1<<6)){printf("OTSD\n\r");}
    if(val1 & (1<<5)){printf("VDS_HA\n\r");}
    if(val1 & (1<<4)){printf("VDS_LA\n\r");}
    if(val1 & (1<<3)){printf("VDS_HB\n\r");}
    if(val1 & (1<<2)){printf("VDS_LB\n\r");}
    if(val1 & (1<<1)){printf("VDS_HC\n\r");}
    if(val1 & (1)){printf("VDS_LC\n\r");}

    if(val2 & (1<<10)){printf("SA_OC\n\r");}
    if(val2 & (1<<9)){printf("SB_OC\n\r");}
    if(val2 & (1<<8)){printf("SC_OC\n\r");}
    if(val2 & (1<<7)){printf("OTW\n\r");}
    if(val2 & (1<<6)){printf("CPUV\n\r");}
    if(val2 & (1<<5)){printf("VGS_HA\n\r");}
    if(val2 & (1<<4)){printf("VGS_LA\n\r");}
    if(val2 & (1<<3)){printf("VGS_HB\n\r");}
    if(val2 & (1<<2)){printf("VGS_LB\n\r");}
    if(val2 & (1<<1)){printf("VGS_HC\n\r");}
    if(val2 & (1)){printf("VGS_LC\n\r");}

}

static int drv_verify_configuration(DRVStruct *drv)
{
	/* DRV8323 returns a read result on the next SPI frame. Read each register
	 * independently and consume the pipeline explicitly; this avoids depending
	 * on stale data left by a preceding write or on a particular burst layout. */
	static const uint8_t registers[] = {FSR1, FSR2, DCR, CSACR, OCPCR};
	uint16_t rx[6] = {0U};
	int ok = 1;

	for (unsigned i = 0U; i < sizeof(registers) / sizeof(registers[0]); ++i) {
		uint16_t discard = 0U;
		const uint16_t command = (uint16_t)(0x8000U | (registers[i] << 11));
		if (drv_spi_transfer(drv, command, &discard) != SPI_TRANSFER_OK ||
		    drv_spi_transfer(drv, command, &rx[i + 1U]) != SPI_TRANSFER_OK) {
			ok = 0;
			drv_init_reason_value |= DRV_INIT_REASON_SPI;
		}
	}
	for (unsigned i = 0U; i < 6U; ++i)
		drv_init_spi_rx_value[i] = rx[i];

	drv->fsr1 = rx[1];
	drv->fsr2 = rx[2];
	drv_init_final_fsr_value = (uint32_t)rx[1] | ((uint32_t)rx[2] << 16);
	drv_init_dcr_csacr_value = (uint32_t)rx[3] |
	                           ((uint32_t)rx[4] << 16);
	drv_init_ocpcr_value = rx[5];
	if (rx[1] != 0U || rx[2] != 0U ||
	    (rx[3] & 0x07FFU) != DRV_DIAG_DCR_VALUE ||
	    rx[4] != (I_MAX <= 40.0f ? DRV_DIAG_CSACR_VALUE_40A :
	                              DRV_DIAG_CSACR_VALUE_60A) ||
	    rx[5] != DRV_DIAG_OCPCR_VALUE) {
		ok = 0;
		drv_init_reason_value |= DRV_INIT_REASON_READBACK;
	}
	return ok ? 0 : -1;
}

static uint32_t drv_capture_fsr_pair(DRVStruct *drv)
{
	uint16_t rx0 = 0U, rx1 = 0U, rx2 = 0U;
	int s0 = drv_spi_transfer(drv, (uint16_t)(0x8000U | (FSR1 << 11)), &rx0);
	int s1 = drv_spi_transfer(drv, (uint16_t)(0x8000U | (FSR2 << 11)), &rx1);
	int s2 = drv_spi_transfer(drv, (uint16_t)(0x8000U | (FSR2 << 11)), &rx2);
	(void)rx0;
	if (s0 != SPI_TRANSFER_OK || s1 != SPI_TRANSFER_OK ||
	    s2 != SPI_TRANSFER_OK) {
		drv_init_reason_value |= DRV_INIT_REASON_SPI;
		/* A failed transaction used to leave the zero-initialized receive words
		 * indistinguishable from a genuine clear FSR pair. */
		return 0xFFFFFFFFU;
	}
	return (uint32_t)rx1 | ((uint32_t)rx2 << 16);
}

int drv_init_config(DRVStruct drv)
{
	/* DRV8323 setup */

#ifdef SAFE_BRINGUP
	(void)drv;
	safety_force_outputs_off(SAFETY_FAULT_SAFE_BRINGUP);
	return -1;
#else
	/* Start from a true electrical idle. PA11 is raised only for the bounded
	 * DRV register setup and is lowered again before the application loop. */
	drv_init_reason_value = 0U;
	drv_init_pre_fsr_value = 0U;
	drv_init_final_fsr_value = 0U;
	drv_init_dcr_csacr_value = 0U;
	drv_init_ocpcr_value = 0U;
	for (unsigned i = 0U; i < 6U; ++i)
		drv_init_spi_rx_value[i] = 0U;
	safety_outputs_off();

	// Up to 40A use 40X amplifier gain
	// From 40-60A use 20X amplifier gain.  (Make this generic in the future)
	int CSA_GAIN = I_MAX <= 40.0f ? CSA_GAIN_40 : CSA_GAIN_20;

	/* The validated bring-up image keeps this bounded window open while the
	 * charge pump settles. nFAULT edges here are evidence, not an application
	 * safety trip: the final FSR/readback gate below decides success. */
	drv_init_window = 1U;
	gpio_bit_set(ENABLE_PIN);
	delay_1ms(DRV_ENABLE_SETTLE_MS);
	drv_init_pre_fsr_value = drv_capture_fsr_pair(&drv);
	if (drv_init_pre_fsr_value != 0U)
		drv_init_reason_value |= DRV_INIT_REASON_PRE_FAULT;

	// Driver Control, Gate drive fault is disabled, 3x PWM mode, clear latched fault bits
	drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);

	// CSA Control, VREF_DIV=2, CSA_GAIN, CSA_CAL_A/B/C, SEN_LVL=0.25v
	/* Do not issue the legacy CSA_CAL_A/B/C pulse during normal startup. The
	 * validated DRV wake sequence configures CSACR directly; on this board the
	 * separate calibration write can assert VGS_HA/OTW before the final readback
	 * and permanently gate the application. Configure the final gain before
	 * sampling ADC offsets so the offset and scale always describe the same CSA
	 * state on both boot and re-arm paths. */
	// CSA Control, VREF_DIV=2, CSA_GAIN, DIS_SEN, SEN_LVL=0.25v
	drv_write_register(drv, CSACR,
		CSA_GAIN == CSA_GAIN_40 ? DRV_DIAG_CSACR_VALUE_40A :
		                          DRV_DIAG_CSACR_VALUE_60A);
	zero_current(&controller);

	// OCP Contro, TRETRY=50us, DEAD_TIME=50us, OCP_MODE=retry, OCP_DEG=4us, VDS_LVL=0.45v
	drv_write_register(drv, OCPCR, DRV_DIAG_OCPCR_VALUE);

	/* The CSA calibration transaction can assert a transient CPUV/VGS fault
	 * while the charge pump settles. The validated wake path clears latched
	 * DRV faults after the complete register set, then performs its final
	 * continuous readback. Keep the same ordering in the normal image. */
	drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);
	delay_1ms(1U);
	/* DRV8323 CSA output bias can move when OCP/DCR and the charge pump settle.
	 * The first sample above establishes the chain, but only this post-settle
	 * sample matches the configuration used during normal commutation. */
	zero_current(&controller);

	/* Do not expose the application state machine until the same readback gate
	 * used by the validated diagnostic wake has passed. */
	if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET)
		drv_init_reason_value |= DRV_INIT_REASON_FINAL_FAULT;
	if (drv_verify_configuration(&drv) != 0 ||
	    gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		/* A DRV fault may be latched by the first charge-pump attempt even
		 * after nFAULT has returned high. Give CLR_FLT one bounded retry while
		 * PWM/POEN remain disabled; persistent FSR bits still fail closed. */
		drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);
		delay_1ms(1U);
		if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) != RESET &&
		    drv_verify_configuration(&drv) == 0) {
			drv_init_reason_value &= ~DRV_INIT_REASON_READBACK;
		} else {
			drv_init_window = 0U;
			safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
			drv_disable_gd(drv);
			return -1;
		}
	}
	if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		drv_init_window = 0U;
		safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
		drv_disable_gd(drv);
		return -1;
	}

	// all MOSFETs in the Hi-Z state, disable output
	drv_disable_gd(drv);
	drv_init_window = 0U;
	safety_outputs_off();
	/* Keep the offsets measured with EN_GATE asserted and the CSA configured.
	 * With the DRV disabled the shunt amplifiers are no longer in the same
	 * common-mode state as a live three-PWM bridge.  Re-sampling in Hi-Z was
	 * observed as a stable fictitious approximately +4 A/-2 A phase current;
	 * the first live current-control cycle then commanded a compensating vector
	 * before the requested MIT torque could take effect. */
	return 0;
#endif
}

void drv_clear_fault(DRVStruct drv)
{
	uint16_t val = (drv_read_register(drv, DCR)) | CLR_FLT_RST;
	drv_write_register(drv, DCR, val);
}

void MX_EXTI_Init()
{
	gpio_exti_source_select(GPIO_PORT_SOURCE_GPIOA, GPIO_PIN_SOURCE_12);

	exti_init(EXTI_12, EXTI_INTERRUPT, EXTI_TRIG_BOTH);
	exti_interrupt_flag_clear(EXTI_12);
}
