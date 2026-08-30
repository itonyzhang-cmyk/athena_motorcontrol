/*
 * position_sensor.h
 *
 *  Created on: Jul 26, 2020
 *      Author: Ben
 */

#ifndef INC_POSITION_SENSOR_H_
#define INC_POSITION_SENSOR_H_


#include "spi.h"
#include <stdint.h>

/* At the 30 kHz FOC rate, a 20-sample endpoint difference makes one AS5047
 * count appear as roughly 0.60 rad/s.  The 128-sample window reduces that
 * quantisation step to about 0.094 rad/s without changing motor-side position
 * or MIT units. */
#define N_POS_SAMPLES 128
#define N_LUT 128

typedef struct{
	union{
		uint8_t spi_tx_buff[2];
		uint16_t spi_tx_word;
	};
	union{
		uint8_t spi_rx_buff[2];
		uint16_t spi_rx_word;
	};
	float angle_singleturn, old_angle, angle_multiturn[N_POS_SAMPLES], elec_angle, velocity, elec_velocity, ppairs, vel2;
	float output_angle_multiturn;
	int raw, count, old_count, turns;
	int count_buff[N_POS_SAMPLES];
	int m_zero, e_zero;
	int offset_lut[N_LUT];
	uint8_t first_sample;
	uint8_t valid;
	uint8_t diagnostics_valid;
	uint16_t last_frame;
	uint16_t raw14;
	uint16_t last_raw14;
	uint16_t diaagc;
	uint16_t magnitude;
	uint16_t error_flags;
	uint16_t consecutive_errors;
	uint32_t sample_count;
	uint32_t valid_count;
	uint32_t invalid_count;
	uint32_t spi_timeout_count;
	uint32_t parity_error_count;
	uint32_t sensor_error_count;
	uint32_t jump_error_count;
	uint32_t unchanged_count;
} EncoderStruct;


void ps_warmup(EncoderStruct * encoder, int n);
void ps_sample(EncoderStruct * encoder, float dt);
void ps_print(EncoderStruct * encoder, int dt_ms);
int ps_read_diagnostics(EncoderStruct *encoder);
int ps_clear_errors(EncoderStruct *encoder);

#endif /* INC_POSITION_SENSOR_H_ */
