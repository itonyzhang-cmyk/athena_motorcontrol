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

#define DRV_ENABLE_SETTLE_MS 10U
#define DRV_DCR_CONFIG_VALUE 0x00A1U

#ifndef STM32F446
static volatile uint8_t drv_enable_pending;
static volatile uint8_t drv_enable_verified;
static uint32_t drv_enable_started_ms;
static int drv_verify_configuration(DRVStruct *drv);
#endif

static volatile uint8_t drv_init_window;
static volatile uint32_t drv_init_reason_value;
static volatile uint32_t drv_init_pre_fsr_value;
static volatile uint32_t drv_init_final_fsr_value;
static volatile uint32_t drv_init_dcr_csacr_value;
static volatile uint32_t drv_init_ocpcr_value;

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
	drv_enable_started_ms = systick_uptime_ms();
	drv_enable_pending = 1U;
	drv_enable_verified = 0U;
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
	if (drv_enable_pending == 0U ||
	    !motor_gate_precharge_complete(drv_enable_started_ms,
	                                   systick_uptime_ms(),
	                                   DRV_ENABLE_SETTLE_MS)) {
		return;
	}
	drv_enable_pending = 0U;

	/* EN_GATE can reset the DRV register bank while low. Reapply the complete
	 * runtime configuration after every enable, with POEN still disabled. */
	if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
		drv.fault = 1U;
		return;
	}
	drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);
	drv_write_register(drv, CSACR,
		I_MAX <= 40.0f ? DRV_DIAG_CSACR_VALUE_40A : DRV_DIAG_CSACR_VALUE_60A);
	drv_write_register(drv, OCPCR, DRV_DIAG_OCPCR_VALUE);
	if (drv_verify_configuration(&drv) != 0 ||
	    gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
		drv.fault = 1U;
		return;
	}

	drv_enable_verified = 1U;
	timer_channel_output_state_config(TIM_PWM, TIM_CH_U, TIMER_CCX_ENABLE);
	timer_channel_output_state_config(TIM_PWM, TIM_CH_V, TIMER_CCX_ENABLE);
	timer_channel_output_state_config(TIM_PWM, TIM_CH_W, TIMER_CCX_ENABLE);
	timer_primary_output_config(TIM_PWM, ENABLE);
#endif
}

int drv_enable_ready(void)
{
#ifdef STM32F446
	return 1;
#else
	return drv_enable_verified != 0U;
#endif
}

void drv_disable_gd(DRVStruct drv){
	/* Stop switching and remove the independent gate-enable immediately. This
	 * path is used by timeout, mode exit and startup idle; no disabled-DRV SPI
	 * access is attempted. */
#ifndef STM32F446
	(void)drv;
	timer_primary_output_config(TIM_PWM, DISABLE);
	gpio_bit_reset(ENABLE_PIN);
	drv_enable_pending = 0U;
	drv_enable_verified = 0U;
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
	/* DRV8323 read responses are returned on the following SPI frame. Keep the
	 * complete sequence explicit so a stale response can never be mistaken for
	 * a successful readback. */
	static const uint16_t commands[] = {
		(uint16_t)(0x8000U | (FSR1 << 11)),
		(uint16_t)(0x8000U | (FSR2 << 11)),
		(uint16_t)(0x8000U | (DCR << 11)),
		(uint16_t)(0x8000U | (CSACR << 11)),
		(uint16_t)(0x8000U | (OCPCR << 11)),
		(uint16_t)(0x8000U | (OCPCR << 11))
	};
	uint16_t rx[sizeof(commands) / sizeof(commands[0])] = {0U};
	int ok = 1;

	for (unsigned i = 0U; i < sizeof(commands) / sizeof(commands[0]); ++i) {
		if (drv_spi_transfer(drv, commands[i], &rx[i]) != SPI_TRANSFER_OK) {
			ok = 0;
			drv_init_reason_value |= DRV_INIT_REASON_SPI;
		}
	}

	/* rx[0] is the response to the command before this sequence. */
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
	    s2 != SPI_TRANSFER_OK)
		drv_init_reason_value |= DRV_INIT_REASON_SPI;
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
	 * and permanently gate the application. Current offsets are established by
	 * the ADC path below instead. */
	zero_current(&controller);

	// CSA Control, VREF_DIV=2, CSA_GAIN, DIS_SEN, SEN_LVL=0.25v
	drv_write_register(drv, CSACR,
		CSA_GAIN == CSA_GAIN_40 ? DRV_DIAG_CSACR_VALUE_40A :
		                          DRV_DIAG_CSACR_VALUE_60A);

	// OCP Contro, TRETRY=50us, DEAD_TIME=50us, OCP_MODE=retry, OCP_DEG=4us, VDS_LVL=0.45v
	drv_write_register(drv, OCPCR, DRV_DIAG_OCPCR_VALUE);

	/* The CSA calibration transaction can assert a transient CPUV/VGS fault
	 * while the charge pump settles. The validated wake path clears latched
	 * DRV faults after the complete register set, then performs its final
	 * continuous readback. Keep the same ordering in the normal image. */
	drv_write_register(drv, DCR, DRV_DCR_CONFIG_VALUE);
	delay_1ms(1U);

	/* Do not expose the application state machine until the same readback gate
	 * used by the validated diagnostic wake has passed. */
	if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET)
		drv_init_reason_value |= DRV_INIT_REASON_FINAL_FAULT;
	if (drv_verify_configuration(&drv) != 0 ||
	    gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
		drv_init_window = 0U;
		safety_force_outputs_off(SAFETY_FAULT_GATE_DRIVER);
		drv_disable_gd(drv);
		return -1;
	}

	// all MOSFETs in the Hi-Z state, disable output
	drv_disable_gd(drv);
	drv_init_window = 0U;
	safety_outputs_off();
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
