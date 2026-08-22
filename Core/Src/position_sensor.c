/*
 * position_sensor.c
 *
 *  Created on: Jul 26, 2020
 *      Author: Ben
 */
#include <stdio.h>
#include <string.h>
#include "position_sensor.h"
#include "math_ops.h"
#include "hw_config.h"
#include "user_config.h"
#include "safety.h"
#include "as5047_protocol.h"
#include "systick.h"

#define AS5047_REG_ERRFL	0x0001
#define AS5047_REG_DIAAGC	0x3FFC
#define AS5047_REG_MAG		0x3FFD
#define AS5047_REG_ANGLECOM	0x3FFF
#define AS5047_REG_CLRERR	0x0001
#define AS5047_MAX_SAMPLE_DELTA 512

#ifndef STM32F446
static int as5047_exchange(uint16_t command, uint16_t *response)
{
	int status;

	SPI_SET_NSS_LOW(ENC_CS);
	spi_chip_select_delay();
	status = spi_transmit_receive(ENC_SPI, &command, response);
	spi_chip_select_delay();
	SPI_SET_NSS_HIGH(ENC_CS);
	spi_chip_select_delay();

	return status;
}

static int as5047_frame_valid(EncoderStruct *encoder, uint16_t frame)
{
	const int status = as5047_response_status(frame);
	encoder->last_frame = frame;

	if (status == -1) {
		encoder->parity_error_count++;
		return 0;
	}

	if (status == -2) {
		encoder->sensor_error_count++;
		return 0;
	}

	return 1;
}

static int as5047_read_register(EncoderStruct *encoder, uint16_t address,
								uint16_t *value)
{
	uint16_t discard;
	uint16_t response;
	const uint16_t command = as5047_make_read_command(address);
	const uint16_t angle_command = as5047_make_read_command(AS5047_REG_ANGLECOM);

	/* AS5047 SPI is pipelined: the response belongs to the previous command. */
	if (as5047_exchange(command, &discard) != SPI_TRANSFER_OK ||
		as5047_exchange(angle_command, &response) != SPI_TRANSFER_OK) {
		encoder->spi_timeout_count++;
		safety_force_outputs_off(SAFETY_FAULT_SPI_TIMEOUT);
		return -1;
	}

	if (!as5047_frame_valid(encoder, response)) {
		return -2;
	}

	*value = response & AS5047_DATA_MASK;

	/* One more angle command consumes the pending angle response and leaves the
	 * pipeline primed for the normal one-transfer-per-sample path. */
	if (as5047_exchange(angle_command, &discard) != SPI_TRANSFER_OK) {
		encoder->spi_timeout_count++;
		safety_force_outputs_off(SAFETY_FAULT_SPI_TIMEOUT);
		return -1;
	}

	return 0;
}
#endif

int ps_clear_errors(EncoderStruct *encoder)
{
#ifdef STM32F446
	(void)encoder;
	return -1;
#else
	uint16_t discard;
	const uint16_t clear_command = as5047_make_write_command(AS5047_REG_CLRERR, 0U);
	const uint16_t angle_command = as5047_make_read_command(AS5047_REG_ANGLECOM);
	/* AS5047 commands are pipelined; consume the write response and prime the
	 * normal angle-read pipeline before sampling starts. */
	if (as5047_exchange(clear_command, &discard) != SPI_TRANSFER_OK ||
	    as5047_exchange(angle_command, &discard) != SPI_TRANSFER_OK) {
		encoder->spi_timeout_count++;
		safety_force_outputs_off(SAFETY_FAULT_SPI_TIMEOUT);
		return -1;
	}
	return 0;
#endif
}

void ps_warmup(EncoderStruct * encoder, int n){
#ifdef STM32F446
	/* Hall position sensors noisy on startup.  Take a bunch of samples to clear this data */
	for(int i = 0; i<n; i++){
		encoder->spi_tx_word = 0x0000;
		HAL_GPIO_WritePin(ENC_CS, GPIO_PIN_RESET ); 	// CS low
		HAL_SPI_TransmitReceive(&ENC_SPI, (uint8_t*)encoder->spi_tx_buff, (uint8_t *)encoder->spi_rx_buff, 1, 100);
		while( ENC_SPI.State == HAL_SPI_STATE_BUSY );  					// wait for transmission complete
		HAL_GPIO_WritePin(ENC_CS, GPIO_PIN_SET ); 	// CS high
	}
#else
	const uint16_t angle_command = as5047_make_read_command(AS5047_REG_ANGLECOM);
	uint32_t consecutive_good = 0U;
	int warmup_ok = 0;
	int saw_spi_timeout = 0;
	(void)ps_clear_errors(encoder);

	/* A debugger attach changes reset timing.  Absorb that startup race with a
	 * bounded retry, while keeping all gate outputs disabled. */
	for (unsigned attempt = 0U; attempt < 3U && !warmup_ok; ++attempt) {
		encoder->valid = 0U;
		encoder->diagnostics_valid = 0U;
		encoder->consecutive_errors = 0U;
		consecutive_good = 0U;

		for (int i = 0; i < n; i++) {
			uint16_t response;
			if (as5047_exchange(angle_command, &response) != SPI_TRANSFER_OK) {
				encoder->spi_timeout_count++;
				encoder->consecutive_errors++;
				encoder->valid = 0U;
				consecutive_good = 0U;
				saw_spi_timeout = 1;
				continue;
			}

			/* The first response is for the command that preceded warm-up. */
			if (i > 0 && as5047_frame_valid(encoder, response)) {
				encoder->raw14 = response & AS5047_DATA_MASK;
				encoder->valid = 1U;
				encoder->consecutive_errors = 0U;
				consecutive_good++;
			} else if (i > 0) {
				encoder->valid = 0U;
				consecutive_good = 0U;
			}
		}
		warmup_ok = consecutive_good >= 32U;
		if (!warmup_ok && attempt < 2U) {
			delay_1ms(20U);
		}
	}

	if (!warmup_ok) {
		encoder->valid = 0U;
		safety_force_outputs_off(saw_spi_timeout ? SAFETY_FAULT_SPI_TIMEOUT
		                                          : SAFETY_FAULT_ENCODER);
	}

	encoder->diagnostics_valid = (ps_read_diagnostics(encoder) == 0) ? 1U : 0U;

#endif
}

int ps_read_diagnostics(EncoderStruct *encoder)
{
#ifdef STM32F446
	(void)encoder;
	return -1;
#else
	uint16_t diaagc;
	uint16_t magnitude;
	uint16_t error_flags;

	if (as5047_read_register(encoder, AS5047_REG_DIAAGC, &diaagc) != 0 ||
		as5047_read_register(encoder, AS5047_REG_MAG, &magnitude) != 0 ||
		as5047_read_register(encoder, AS5047_REG_ERRFL, &error_flags) != 0) {
		encoder->diagnostics_valid = 0U;
		return -1;
	}

	encoder->diaagc = diaagc;
	encoder->magnitude = magnitude;
	encoder->error_flags = error_flags;
	encoder->diagnostics_valid = 1U;
	return 0;
#endif
}

void ps_sample(EncoderStruct * encoder, float dt){
	/* updates EncoderStruct encoder with the latest sample
	 * after elapsed time dt */

	/* SPI read/write */
#ifdef STM32F446
	encoder->spi_tx_word = ENC_READ_WORD;
	HAL_GPIO_WritePin(ENC_CS, GPIO_PIN_RESET ); 	// CS low
	HAL_SPI_TransmitReceive(&ENC_SPI, (uint8_t*)encoder->spi_tx_buff, (uint8_t *)encoder->spi_rx_buff, 1, 100);
	while( ENC_SPI.State == HAL_SPI_STATE_BUSY );  					// wait for transmission complete
	HAL_GPIO_WritePin(ENC_CS, GPIO_PIN_SET ); 	// CS high
	encoder->raw = encoder ->spi_rx_word;
#else
	const uint16_t angle_command = as5047_make_read_command(AS5047_REG_ANGLECOM);
	uint16_t response;
	encoder->sample_count++;

	if (as5047_exchange(angle_command, &response) != SPI_TRANSFER_OK) {
		encoder->spi_timeout_count++;
		encoder->invalid_count++;
		encoder->consecutive_errors++;
		encoder->valid = 0U;
		safety_force_outputs_off(SAFETY_FAULT_SPI_TIMEOUT);
		return;
	}

	if (!as5047_frame_valid(encoder, response)) {
		encoder->invalid_count++;
		encoder->consecutive_errors++;
		encoder->valid = 0U;
		if (encoder->consecutive_errors >= 3U) {
			safety_force_outputs_off(SAFETY_FAULT_ENCODER);
		}
		return;
	}

	const uint16_t raw14 = response & AS5047_DATA_MASK;
	if (encoder->valid_count > 0U) {
		const int delta = as5047_wrapped_delta(raw14, encoder->last_raw14);

		if (delta > AS5047_MAX_SAMPLE_DELTA || delta < -AS5047_MAX_SAMPLE_DELTA) {
			encoder->jump_error_count++;
			encoder->invalid_count++;
			encoder->consecutive_errors++;
			encoder->valid = 0U;
			if (encoder->consecutive_errors >= 3U) {
				safety_force_outputs_off(SAFETY_FAULT_ENCODER);
			}
			return;
		}

		if (raw14 == encoder->last_raw14) {
			encoder->unchanged_count++;
		}
	}

	encoder->raw14 = raw14;
	encoder->last_raw14 = raw14;
	encoder->raw = raw14 << 2;
	encoder->valid = 1U;
	encoder->valid_count++;
	encoder->consecutive_errors = 0U;
#endif

	/* Shift previous position samples only after accepting a valid frame. */
	encoder->old_angle = encoder->angle_singleturn;
	for(int i = N_POS_SAMPLES-1; i>0; i--){encoder->angle_multiturn[i] = encoder->angle_multiturn[i-1];}

	/* Linearization */
	int off_1 = encoder->offset_lut[(encoder->raw)>>9];				// lookup table lower entry
	int off_2 = encoder->offset_lut[((encoder->raw>>9)+1)%128];		// lookup table higher entry
	int off_interp = off_1 + ((off_2 - off_1)*(encoder->raw - ((encoder->raw>>9)<<9))>>9);     // Interpolate between lookup table entries
	encoder->count = encoder->raw + off_interp;


	/* Real angles in radians */
	encoder->angle_singleturn = ((float)(encoder->count-M_ZERO))/((float)ENC_CPR);
	int int_angle = encoder->angle_singleturn;
	encoder->angle_singleturn = TWO_PI_F*(encoder->angle_singleturn - (float)int_angle);
	//encoder->angle_singleturn = TWO_PI_F*fmodf(((float)(encoder->count-M_ZERO))/((float)ENC_CPR), 1.0f);
	encoder->angle_singleturn = encoder->angle_singleturn<0 ? encoder->angle_singleturn + TWO_PI_F : encoder->angle_singleturn;

	encoder->elec_angle = (encoder->ppairs*(float)(encoder->count-E_ZERO))/((float)ENC_CPR);
	int_angle = (int)encoder->elec_angle;
	encoder->elec_angle = TWO_PI_F*(encoder->elec_angle - (float)int_angle);
	//encoder->elec_angle = TWO_PI_F*fmodf((encoder->ppairs*(float)(encoder->count-E_ZERO))/((float)ENC_CPR), 1.0f);
	encoder->elec_angle = encoder->elec_angle<0 ? encoder->elec_angle + TWO_PI_F : encoder->elec_angle;	// Add 2*pi to negative numbers
	/* Rollover */
	int rollover = 0;
	float angle_diff = encoder->angle_singleturn - encoder->old_angle;
	if(angle_diff > PI_F){rollover = -1;}
	else if(angle_diff < -PI_F){rollover = 1;}
	encoder->turns += rollover;
	if(!encoder->first_sample){
		encoder->turns = 0;
		encoder->first_sample = 1;
	}



	/* Multi-turn position */
	encoder->angle_multiturn[0] = encoder->angle_singleturn + TWO_PI_F*(float)encoder->turns;

	/* Velocity */
	/*
	// Attempt at a moving least squares.  Wasn't any better
		float m = (float)N_POS_SAMPLES;
		float w = 1.0f/m;
		float q = 12.0f/(m*m*m - m);
		float c1 = 0.0f;
		float ibar = (m - 1.0f)/2.0f;
		for(int i = 0; i<N_POS_SAMPLES; i++){
			c1 += encoder->angle_multiturn[i]*q*(i - ibar);
		}
		encoder->vel2 = -c1/dt;
*/
	//encoder->velocity = vel2
	encoder->velocity = (encoder->angle_multiturn[0] - encoder->angle_multiturn[N_POS_SAMPLES-1])/(dt*(float)(N_POS_SAMPLES-1));
	encoder->elec_velocity = encoder->ppairs*encoder->velocity;

}

void ps_print(EncoderStruct * encoder, int loop_count){

	#define PS_PRINT_INTERVAL 10000
	static uint32_t ps_print_mark = 0;

	if (loop_count < ps_print_mark + PS_PRINT_INTERVAL) {
		return;
	}

	printf("Raw: %5d", encoder->raw);
	printf("  Linearized Count: %5d", encoder->count);
	printf("  Single Turn:% 4.3f", encoder->angle_singleturn);
	printf("  Multi Turn:% 5.3f", encoder->angle_multiturn[0]);
	printf("  Electrical:% 5.3f", encoder->elec_angle);
	printf("  Turns:% 2d", encoder->turns);
	printf("  Valid:%u", encoder->valid);
	printf("  SPI:%lu", (unsigned long)encoder->spi_timeout_count);
	printf("  Parity:%lu", (unsigned long)encoder->parity_error_count);
	printf("  EF:%lu\r\n", (unsigned long)encoder->sensor_error_count);

	ps_print_mark = loop_count;
}
