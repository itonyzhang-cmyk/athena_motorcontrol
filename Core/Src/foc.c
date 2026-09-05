/*
 * foc.c
 *
 *  Created on: Aug 2, 2020
 *      Author: ben
 */

#include "foc.h"
#include "structs.h"
#include <math.h>

#ifndef STM32F446
/* Internal current-loop step test.  CAN only arms it; the 30 kHz ISR owns
 * the waveform and metrics so bridge timing cannot affect the experiment. */
static volatile uint8_t current_test_active;
static volatile uint32_t current_test_tick;
static volatile float current_test_step;
static volatile float current_test_peak;
static volatile float current_test_min;
static volatile float current_test_final;
static volatile float current_test_other_peak;
static volatile float current_test_other_min;
static volatile float current_test_start_theta_mech;
static volatile float current_test_start_theta_elec;
static volatile float current_test_final_theta_mech;
static volatile float current_test_final_theta_elec;
static volatile uint8_t current_test_seen;
static volatile int16_t current_test_start_raw_b, current_test_start_raw_c;
static volatile int16_t current_test_start_i_b, current_test_start_i_c;
static volatile int16_t current_test_start_i_d, current_test_start_i_q;
static volatile int16_t current_test_step_i_b, current_test_step_i_c;
static volatile int16_t current_test_step_i_d, current_test_step_i_q;
static volatile int16_t current_test_final_raw_b, current_test_final_raw_c;
static volatile int16_t current_test_final_i_b, current_test_final_i_c;
static volatile int16_t current_test_final_i_d, current_test_final_i_q;
static volatile uint32_t current_test_peak_tick, current_test_min_tick;
static volatile int16_t current_test_peak_raw_b, current_test_peak_raw_c;
static volatile int16_t current_test_peak_i_d, current_test_peak_i_q;
static volatile int16_t current_test_min_raw_b, current_test_min_raw_c;
static volatile int16_t current_test_min_i_d, current_test_min_i_q;
static volatile int16_t current_test_peak_v_d, current_test_peak_v_q;
static volatile int16_t current_test_peak_dtc_u, current_test_peak_dtc_v, current_test_peak_dtc_w;
static volatile int16_t current_test_min_v_d, current_test_min_v_q;
static volatile int16_t current_test_min_dtc_u, current_test_min_dtc_v, current_test_min_dtc_w;
static volatile int16_t current_test_offset_b, current_test_offset_c;
static volatile int16_t current_test_final_dtheta_elec;
static volatile int16_t current_test_final_v_max;
static volatile int16_t current_test_final_v_d, current_test_final_v_q;
static volatile int16_t current_test_final_d_int, current_test_final_q_int;
static volatile int16_t current_test_final_v_ref;
static volatile uint8_t current_test_axis;
#define ADC_BASELINE_SAMPLES 1000U
static volatile uint8_t adc_baseline_active;
static volatile uint32_t adc_baseline_count, adc_baseline_valid;
static volatile uint32_t adc_baseline_sum_b, adc_baseline_sum_c, adc_baseline_sum_vbus;
static volatile uint16_t adc_baseline_min_b, adc_baseline_min_c, adc_baseline_min_vbus;
static volatile uint16_t adc_baseline_max_b, adc_baseline_max_c, adc_baseline_max_vbus;

/* Live CSA offset capture runs from the timer-driven ADC path.  It must not
 * block the 30 kHz ISR waiting for future UPDATE events, so the enable
 * sequence starts this accumulator and the following UPDATE samples finish
 * it. */
#define LIVE_OFFSET_SAMPLES 64U
static volatile uint8_t live_offset_active;
static volatile uint8_t live_offset_complete;
static volatile uint16_t live_offset_count;
static volatile uint32_t live_offset_sum_b, live_offset_sum_c;
static volatile uint16_t live_offset_avg_b, live_offset_avg_c;

void adc_baseline_test_start(void)
{
    adc_baseline_count = adc_baseline_valid = 0U;
    adc_baseline_sum_b = adc_baseline_sum_c = adc_baseline_sum_vbus = 0U;
    adc_baseline_min_b = adc_baseline_min_c = adc_baseline_min_vbus = 0xFFFFU;
    adc_baseline_max_b = adc_baseline_max_c = adc_baseline_max_vbus = 0U;
    adc_baseline_active = 1U;
}
uint8_t adc_baseline_test_active(void) { return adc_baseline_active; }
uint32_t adc_baseline_test_snapshot(uint8_t page)
{
    uint32_t n = adc_baseline_count ? adc_baseline_count : 1U;
    switch (page) {
    case 247U: return (uint32_t)adc_baseline_active | (adc_baseline_count << 8);
    /* Pages 248..251 are physical ADC0/ADC1 values.  They deliberately do
     * not follow PHASE_ORDER, so the two configurations can be compared. */
    case 248U: return adc_baseline_min_b | ((uint32_t)adc_baseline_max_b << 16);
    case 249U: return adc_baseline_min_c | ((uint32_t)adc_baseline_max_c << 16);
    case 250U: return adc_baseline_sum_b / n;
    case 251U: return adc_baseline_sum_c / n;
    case 252U: return adc_baseline_min_vbus | ((uint32_t)adc_baseline_max_vbus << 16);
    case 253U: return adc_baseline_sum_vbus / n;
    case 254U: return adc_baseline_valid;
    case 255U: return (uint32_t)controller.phase_order;
    default: return 0U;
    }
}

uint8_t zero_current_live_active(void) { return live_offset_active; }

void zero_current_live_cancel(void)
{
    live_offset_active = 0U;
    live_offset_complete = 0U;
    live_offset_count = 0U;
    live_offset_sum_b = 0U;
    live_offset_sum_c = 0U;
}

uint32_t zero_current_live_snapshot(uint8_t page)
{
    switch (page) {
    case 0U:
        return (uint32_t)live_offset_active |
               ((uint32_t)live_offset_count << 8) |
               ((uint32_t)live_offset_complete << 16);
    case 1U:
        return (uint32_t)live_offset_avg_b |
               ((uint32_t)live_offset_avg_c << 16);
    case 2U:
        return live_offset_sum_b;
    case 3U:
        return live_offset_sum_c;
    default:
        return 0U;
    }
}

static void zero_current_live_accumulate(ControllerStruct *controller)
{
    if (live_offset_active == 0U || controller->adc_valid == 0U) return;

    live_offset_sum_b += (uint32_t)controller->adc_b_raw;
    live_offset_sum_c += (uint32_t)controller->adc_c_raw;
    live_offset_count++;
    if (live_offset_count >= LIVE_OFFSET_SAMPLES) {
        live_offset_avg_b = (uint16_t)(live_offset_sum_b / LIVE_OFFSET_SAMPLES);
        live_offset_avg_c = (uint16_t)(live_offset_sum_c / LIVE_OFFSET_SAMPLES);
        controller->adc_b_offset = (int)live_offset_avg_b;
        controller->adc_c_offset = (int)live_offset_avg_c;
        live_offset_active = 0U;
        live_offset_complete = 1U;
    }
}
/* Pages 160..189 hold an edge-aligned D/Q time series; 190..219 hold the
 * matching physical ADC1/ADC0 readings. Thirty 30 kHz samples at a 32-cycle
 * stride cover 31 ms of the PI rise, starting at the reference edge. */
#define CURRENT_TEST_EDGE_SAMPLES 30U
#define CURRENT_TEST_EDGE_START_TICK 300U
#define CURRENT_TEST_EDGE_STRIDE 32U
static volatile int16_t current_test_edge_d[CURRENT_TEST_EDGE_SAMPLES];
static volatile int16_t current_test_edge_q[CURRENT_TEST_EDGE_SAMPLES];
static volatile uint16_t current_test_edge_raw_b[CURRENT_TEST_EDGE_SAMPLES];
static volatile uint16_t current_test_edge_raw_c[CURRENT_TEST_EDGE_SAMPLES];

void current_loop_test_start(float step_amps, uint8_t axis)
{
    if (!(fabsf(step_amps) >= 0.1f && fabsf(step_amps) <= 2.0f) || axis > 1U) return;
    current_test_step = step_amps;
    current_test_axis = axis;
    current_test_tick = 0U;
    current_test_peak = 0.0f;
    current_test_min = 0.0f;
    current_test_other_peak = 0.0f;
    current_test_other_min = 0.0f;
    current_test_start_theta_mech = 0.0f;
    current_test_start_theta_elec = 0.0f;
    current_test_final_theta_mech = 0.0f;
    current_test_final_theta_elec = 0.0f;
    current_test_seen = 0U;
    current_test_start_raw_b = 0;
    current_test_start_raw_c = 0;
    current_test_start_i_b = 0;
    current_test_start_i_c = 0;
    current_test_start_i_d = 0;
    current_test_start_i_q = 0;
    current_test_step_i_b = 0;
    current_test_step_i_c = 0;
    current_test_step_i_d = 0;
    current_test_step_i_q = 0;
    current_test_final_raw_b = 0;
    current_test_final_raw_c = 0;
    current_test_final_i_b = 0;
    current_test_final_i_c = 0;
    current_test_final_i_d = 0;
    current_test_final_i_q = 0;
    current_test_peak_tick = 0U;
    current_test_min_tick = 0U;
    current_test_peak_raw_b = 0;
    current_test_peak_raw_c = 0;
    current_test_peak_i_d = 0;
    current_test_peak_i_q = 0;
    current_test_min_raw_b = 0;
    current_test_min_raw_c = 0;
    current_test_min_i_d = 0;
    current_test_min_i_q = 0;
    current_test_peak_v_d = 0;
    current_test_peak_v_q = 0;
    current_test_peak_dtc_u = 0;
    current_test_peak_dtc_v = 0;
    current_test_peak_dtc_w = 0;
    current_test_min_v_d = 0;
    current_test_min_v_q = 0;
    current_test_min_dtc_u = 0;
    current_test_min_dtc_v = 0;
    current_test_min_dtc_w = 0;
    current_test_final_dtheta_elec = 0;
    current_test_final_v_max = 0;
    current_test_final_v_d = 0;
    current_test_final_v_q = 0;
    current_test_final_d_int = 0;
    current_test_final_q_int = 0;
    current_test_final_v_ref = 0;
    current_test_offset_b = (int16_t)controller.adc_b_offset;
    current_test_offset_c = (int16_t)controller.adc_c_offset;
    /* Each trial is independent.  A preceding step can leave a nonzero PI
     * integrator even after its reference returns to zero; retaining it would
     * energize the nominal 300-cycle baseline of the next trial. */
    controller.i_d_des = 0.0f;
    controller.i_q_des = 0.0f;
    controller.i_d_des_filt = 0.0f;
    controller.i_q_des_filt = 0.0f;
    controller.d_int = 0.0f;
    controller.q_int = 0.0f;
    current_test_final = 0.0f;
    for (uint32_t i = 0U; i < CURRENT_TEST_EDGE_SAMPLES; ++i) {
        current_test_edge_d[i] = 0;
        current_test_edge_q[i] = 0;
        current_test_edge_raw_b[i] = 0U;
        current_test_edge_raw_c[i] = 0U;
    }
    current_test_active = 1U;
}

uint8_t current_loop_test_set_gains(float k_p, float k_i)
{
    /* This is deliberately RAM-only: it supports a controlled tuning sweep
     * without silently changing the persistent motor configuration. */
    if (current_test_active != 0U || !(k_p >= 0.001f && k_p <= 0.250f) ||
        !(k_i >= 0.0f && k_i <= 0.100f)) return 0U;
    controller.k_d = k_p;
    controller.k_q = k_p;
    controller.ki_d = k_i;
    controller.ki_q = k_i;
    controller.d_int = 0.0f;
    controller.q_int = 0.0f;
    return 1U;
}

uint8_t current_loop_test_active(void) { return current_test_active; }

uint32_t current_loop_test_snapshot(uint8_t page)
{
    switch (page) {
    case 150U: return (uint32_t)current_test_active | (current_test_tick << 8);
    case 151U: return (uint32_t)(int32_t)(current_test_step * 1000.0f);
    case 152U: return (uint32_t)(int32_t)(current_test_peak * 1000.0f);
    case 153U: return (uint32_t)(int32_t)(current_test_min * 1000.0f);
    case 154U: return (uint32_t)(int32_t)(current_test_final * 1000.0f);
    case 155U: return current_test_axis;
    case 156U: return (uint32_t)(int32_t)(controller.k_q * 1000000.0f);
    case 157U: return (uint32_t)(int32_t)(controller.ki_q * 1000000.0f);
    /* Latched before the step reference is cleared; these describe the
     * energized current-loop operating point, not post-test idle state. */
    case 158U: return (uint32_t)(int32_t)current_test_final_dtheta_elec;
    case 159U: return (uint32_t)(int32_t)current_test_final_v_max;
    case 147U: return (uint32_t)(uint16_t)current_test_final_v_d |
                       ((uint32_t)(uint16_t)current_test_final_v_q << 16);
    case 148U: return (uint32_t)(uint16_t)current_test_final_d_int |
                       ((uint32_t)(uint16_t)current_test_final_q_int << 16);
    case 149U: return (uint32_t)(int32_t)current_test_final_v_ref;
    /* The non-target axis extrema distinguish real d/q coupling from the
     * q value coincident with a target-axis overshoot. */
    case 220U: return (uint32_t)(int32_t)(current_test_other_peak * 1000.0f);
    case 221U: return (uint32_t)(int32_t)(current_test_other_min * 1000.0f);
    case 222U: return (uint32_t)(int32_t)(current_test_start_theta_mech * 1000.0f);
    case 223U: return (uint32_t)(int32_t)(current_test_start_theta_elec * 1000.0f);
    case 224U: return (uint32_t)(int32_t)(current_test_final_theta_mech * 1000.0f);
    case 225U: return (uint32_t)(int32_t)(current_test_final_theta_elec * 1000.0f);
    case 226U: return (uint32_t)(uint16_t)current_test_start_raw_b |
                       ((uint32_t)(uint16_t)current_test_start_raw_c << 16);
    case 227U: return (uint32_t)(uint16_t)current_test_start_i_b |
                       ((uint32_t)(uint16_t)current_test_start_i_c << 16);
    case 228U: return (uint32_t)(uint16_t)current_test_start_i_d |
                       ((uint32_t)(uint16_t)current_test_start_i_q << 16);
    case 229U: return (uint32_t)(uint16_t)current_test_step_i_b |
                       ((uint32_t)(uint16_t)current_test_step_i_c << 16);
    case 230U: return (uint32_t)(uint16_t)current_test_step_i_d |
                       ((uint32_t)(uint16_t)current_test_step_i_q << 16);
    case 231U: return (uint32_t)(uint16_t)current_test_final_raw_b |
                       ((uint32_t)(uint16_t)current_test_final_raw_c << 16);
    case 232U: return (uint32_t)(uint16_t)current_test_final_i_b |
                       ((uint32_t)(uint16_t)current_test_final_i_c << 16);
    case 233U: return (uint32_t)(uint16_t)current_test_final_i_d |
                       ((uint32_t)(uint16_t)current_test_final_i_q << 16);
    case 234U: return (uint32_t)(uint16_t)current_test_offset_b |
                       ((uint32_t)(uint16_t)current_test_offset_c << 16);
    case 235U: return current_test_peak_tick;
    case 236U: return current_test_min_tick;
    case 237U: return (uint32_t)(uint16_t)current_test_peak_raw_b |
                       ((uint32_t)(uint16_t)current_test_peak_raw_c << 16);
    case 238U: return (uint32_t)(uint16_t)current_test_peak_i_d |
                       ((uint32_t)(uint16_t)current_test_peak_i_q << 16);
    case 239U: return (uint32_t)(uint16_t)current_test_min_raw_b |
                       ((uint32_t)(uint16_t)current_test_min_raw_c << 16);
    case 240U: return (uint32_t)(uint16_t)current_test_min_i_d |
                       ((uint32_t)(uint16_t)current_test_min_i_q << 16);
    case 241U: return (uint32_t)(uint16_t)current_test_peak_v_d |
                       ((uint32_t)(uint16_t)current_test_peak_v_q << 16);
    case 242U: return (uint32_t)(uint16_t)current_test_peak_dtc_u |
                       ((uint32_t)(uint16_t)current_test_peak_dtc_v << 16);
    case 243U: return (uint32_t)(uint16_t)current_test_peak_dtc_w;
    case 244U: return (uint32_t)(uint16_t)current_test_min_v_d |
                       ((uint32_t)(uint16_t)current_test_min_v_q << 16);
    case 245U: return (uint32_t)(uint16_t)current_test_min_dtc_u |
                       ((uint32_t)(uint16_t)current_test_min_dtc_v << 16);
    case 246U: return (uint32_t)(uint16_t)current_test_min_dtc_w;
    default:
        if (page >= 160U && page < 160U + CURRENT_TEST_EDGE_SAMPLES) {
            const uint8_t index = page - 160U;
            return (uint32_t)(uint16_t)current_test_edge_d[index] |
                   ((uint32_t)(uint16_t)current_test_edge_q[index] << 16);
        }
        if (page >= 190U && page < 190U + CURRENT_TEST_EDGE_SAMPLES) {
            const uint8_t index = page - 190U;
            return (uint32_t)current_test_edge_raw_b[index] |
                   ((uint32_t)current_test_edge_raw_c[index] << 16);
        }
        return 0U;
    }
}
#endif
#include "adc.h"
#include "tim.h"
#include "position_sensor.h"
#include "math_ops.h"
#include "hw_config.h"
#include "user_config.h"
#include "safety.h"
#include "ivt_protection.h"

#ifndef STM32F446
#define ADC_EOIC_POLL_LIMIT 2048U

static int adc_wait_for_eoic(uint32_t adc_periph)
{
	uint32_t remaining = ADC_EOIC_POLL_LIMIT;
	while (RESET == adc_flag_get(adc_periph, ADC_FLAG_EOIC)) {
		if (remaining-- == 0U) {
			return -1;
		}
	}
	return 0;
}
#ifdef ADC_SYNC_TRIGGER
#define ADC_SYNC_STALE_LIMIT 4U
#endif
#endif

static void evaluate_i_v_t_protection(ControllerStruct *controller)
{
    const IvtProtectionConfig config = {
        .enabled = (uint32_t)IVT_PROTECT_ENABLE,
        .current_trip_a = I_TRIP,
        .vbus_min_v = VBUS_MIN,
        .vbus_max_v = VBUS_MAX,
        .temperature_trip_c = TEMP_TRIP
    };
    const IvtProtectionSample sample = {
        .i_a = controller->i_a,
        .i_b = controller->i_b,
        .i_c = controller->i_c,
        .vbus_v = controller->v_bus,
        /* The only existing thermal observer is commented out and no physical
         * thermistor conversion has been characterized.  Temperature shutdown
         * is therefore fail-closed if somebody explicitly enables it. */
        .temperature_c = 0.0f,
        .current_valid = controller->adc_valid,
        .vbus_valid = controller->adc_valid,
        .temperature_valid = 0U
    };
    const uint32_t faults = ivt_protection_evaluate(&config, &sample);
    if (faults != SAFETY_FAULT_NONE) {
        safety_force_outputs_off(faults);
    }
}

void set_dtc(ControllerStruct *controller){

	/* Invert duty cycle if that's how hardware is configured */

	float dtc_u = controller->dtc_u;
	float dtc_v = controller->dtc_v;
	float dtc_w = controller->dtc_w;

	if(INVERT_DTC){
		dtc_u = 1.0f - controller->dtc_u;
		dtc_v = 1.0f - controller->dtc_v;
		dtc_w = 1.0f - controller->dtc_w;
	}
	/* Handle phase order swapping so that voltage/current/torque match encoder direction */

#ifdef STM32F446
	if(!PHASE_ORDER){
		__HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_U, ((TIM_PWM.Instance->ARR))*dtc_u);
		__HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_V, ((TIM_PWM.Instance->ARR))*dtc_v);
		__HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_W, ((TIM_PWM.Instance->ARR))*dtc_w);
	}
	else{
		__HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_V, ((TIM_PWM.Instance->ARR))*dtc_u);
		__HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_U, ((TIM_PWM.Instance->ARR))*dtc_v);
		__HAL_TIM_SET_COMPARE(&TIM_PWM, TIM_CH_W, ((TIM_PWM.Instance->ARR))*dtc_w);
	}
#else
	if(!PHASE_ORDER){
		timer_channel_output_pulse_value_config(TIMER0, TIM_CH_U, SVPWM_PERIOD * dtc_u);
		timer_channel_output_pulse_value_config(TIMER0, TIM_CH_V, SVPWM_PERIOD * dtc_v);
		timer_channel_output_pulse_value_config(TIMER0, TIM_CH_W, SVPWM_PERIOD * dtc_w);
	}
	else{
		timer_channel_output_pulse_value_config(TIMER0, TIM_CH_V, SVPWM_PERIOD * dtc_u);
		timer_channel_output_pulse_value_config(TIMER0, TIM_CH_U, SVPWM_PERIOD * dtc_v);
		timer_channel_output_pulse_value_config(TIMER0, TIM_CH_W, SVPWM_PERIOD * dtc_w);
	}
#endif
}

void analog_sample (ControllerStruct *controller){
	/* Sampe ADCs */
	/* Handle phase order swapping so that voltage/current/torque match encoder direction */
#ifdef STM32F446
	if(!PHASE_ORDER){
		controller->adc_a_raw = HAL_ADC_GetValue(&ADC_CH_IA);
		controller->adc_b_raw = HAL_ADC_GetValue(&ADC_CH_IB);
		//adc_ch_ic = ADC_CH_IC;
	}
	else{
		controller->adc_a_raw = HAL_ADC_GetValue(&ADC_CH_IB);
		controller->adc_b_raw = HAL_ADC_GetValue(&ADC_CH_IA);
		//adc_ch_ic = ADC_CH_IB;
	}


	HAL_ADC_Start(&ADC_CH_MAIN);
	HAL_ADC_PollForConversion(&ADC_CH_MAIN, HAL_MAX_DELAY);

	controller->adc_vbus_raw = HAL_ADC_GetValue(&ADC_CH_VBUS);
	controller->v_bus = (float)controller->adc_vbus_raw*V_SCALE;

    controller->i_a = controller->i_scale*(float)(controller->adc_a_raw - controller->adc_a_offset);    // Calculate phase currents from ADC readings
    controller->i_b = controller->i_scale*(float)(controller->adc_b_raw - controller->adc_b_offset);
    controller->i_c = -controller->i_a - controller->i_b;
#else
#ifdef ADC_SYNC_TRIGGER
	/* CH3 triggered this conversion in the preceding PWM half-cycle.  The
	 * first event occurs after timer start, so an initially invalid sample is
	 * not an ADC fault. Once valid, only consecutive missing midpoint samples
	 * take the existing hard-fault path. */
	if (adc_flag_get(ADC_CH_MAIN, ADC_FLAG_EOIC) == RESET ||
		adc_flag_get(ADC_CH_VBUS, ADC_FLAG_EOIC) == RESET) {
		if (controller->adc_valid != 0U &&
			++controller->adc_stale_cycles > ADC_SYNC_STALE_LIMIT) {
			controller->adc_valid = 0U;
			controller->adc_timeout_count++;
			safety_force_outputs_off(SAFETY_FAULT_ADC_TIMEOUT);
		}
		return;
	}
	controller->adc_stale_cycles = 0U;
	adc_flag_clear(ADC_CH_MAIN, ADC_FLAG_EOIC);
	adc_flag_clear(ADC_CH_VBUS, ADC_FLAG_EOIC);
	if(!PHASE_ORDER){
		controller->adc_b_raw = adc_inserted_data_read(ADC_CH_IB, ADC_INSERTED_CHANNEL_0);
		controller->adc_c_raw = adc_inserted_data_read(ADC_CH_IC, ADC_INSERTED_CHANNEL_0);
	}
	else{
		controller->adc_b_raw = adc_inserted_data_read(ADC_CH_IC, ADC_INSERTED_CHANNEL_0);
		controller->adc_c_raw = adc_inserted_data_read(ADC_CH_IB, ADC_INSERTED_CHANNEL_0);
	}
	controller->adc_vbus_raw = adc_inserted_data_read(ADC_CH_VBUS, ADC_INSERTED_CHANNEL_0);
	controller->v_bus = (float)controller->adc_vbus_raw * V_SCALE;
	zero_current_live_accumulate(controller);
	controller->i_b = controller->i_scale * (float)(controller->adc_b_raw - controller->adc_b_offset);
	controller->i_c = controller->i_scale * (float)(controller->adc_c_raw - controller->adc_c_offset);
	controller->i_a = -controller->i_b - controller->i_c;
	controller->adc_valid = 1U;
	controller->adc_sample_count++;
#else
	if(!PHASE_ORDER){
		controller->adc_b_raw = adc_inserted_data_read(ADC_CH_IB, ADC_INSERTED_CHANNEL_0);
		controller->adc_c_raw = adc_inserted_data_read(ADC_CH_IC, ADC_INSERTED_CHANNEL_0);
	}
	else{
		controller->adc_b_raw = adc_inserted_data_read(ADC_CH_IC, ADC_INSERTED_CHANNEL_0);
		controller->adc_c_raw = adc_inserted_data_read(ADC_CH_IB, ADC_INSERTED_CHANNEL_0);
	}

    adc_software_trigger_enable(ADC_CH_MAIN, ADC_INSERTED_CHANNEL);
    adc_software_trigger_enable(ADC_CH_VBUS, ADC_INSERTED_CHANNEL);

    if (adc_wait_for_eoic(ADC_CH_MAIN) != 0 ||
        adc_wait_for_eoic(ADC_CH_VBUS) != 0) {
        adc_flag_clear(ADC_CH_MAIN, ADC_FLAG_EOIC);
        adc_flag_clear(ADC_CH_VBUS, ADC_FLAG_EOIC);
        controller->adc_valid = 0U;
        controller->adc_timeout_count++;
        safety_force_outputs_off(SAFETY_FAULT_ADC_TIMEOUT);
        return;
    }

    adc_flag_clear(ADC_CH_MAIN, ADC_FLAG_EOIC);
    adc_flag_clear(ADC_CH_VBUS, ADC_FLAG_EOIC);

	controller->adc_vbus_raw = adc_inserted_data_read(ADC_CH_VBUS, ADC_INSERTED_CHANNEL_0);
    controller->v_bus = (float)controller->adc_vbus_raw * V_SCALE;

    // Calculate phase currents from ADC readings
    controller->i_b = controller->i_scale * (float)(controller->adc_b_raw - controller->adc_b_offset);
    controller->i_c = controller->i_scale * (float)(controller->adc_c_raw - controller->adc_c_offset);
    controller->i_a = -controller->i_b - controller->i_c;
    controller->adc_valid = 1U;
    controller->adc_sample_count++;
#endif
	if (adc_baseline_active != 0U) {
		/* Diagnostic-only fresh conversion: keep the production FOC sample
		 * path unchanged, then trigger and read a separate sample for timing
		 * characterization. */
		adc_software_trigger_enable(ADC_CH_MAIN, ADC_INSERTED_CHANNEL);
		adc_software_trigger_enable(ADC_CH_VBUS, ADC_INSERTED_CHANNEL);
		if (adc_wait_for_eoic(ADC_CH_MAIN) == 0 && adc_wait_for_eoic(ADC_CH_VBUS) == 0) {
			adc_flag_clear(ADC_CH_MAIN, ADC_FLAG_EOIC);
			adc_flag_clear(ADC_CH_VBUS, ADC_FLAG_EOIC);
			uint16_t b=(uint16_t)adc_inserted_data_read(ADC_CH_IB, ADC_INSERTED_CHANNEL_0);
			uint16_t c=(uint16_t)adc_inserted_data_read(ADC_CH_IC, ADC_INSERTED_CHANNEL_0);
			uint16_t v=(uint16_t)adc_inserted_data_read(ADC_CH_VBUS, ADC_INSERTED_CHANNEL_0);
		if (b<adc_baseline_min_b) adc_baseline_min_b=b;
		if (c<adc_baseline_min_c) adc_baseline_min_c=c;
		if (v<adc_baseline_min_vbus) adc_baseline_min_vbus=v;
		if (b>adc_baseline_max_b) adc_baseline_max_b=b;
		if (c>adc_baseline_max_c) adc_baseline_max_c=c;
		if (v>adc_baseline_max_vbus) adc_baseline_max_vbus=v;
		adc_baseline_sum_b+=b; adc_baseline_sum_c+=c; adc_baseline_sum_vbus+=v; adc_baseline_valid++;
			if (++adc_baseline_count >= ADC_BASELINE_SAMPLES) adc_baseline_active=0U;
		} else {
			adc_flag_clear(ADC_CH_MAIN, ADC_FLAG_EOIC);
			adc_flag_clear(ADC_CH_VBUS, ADC_FLAG_EOIC);
		}
	}

    evaluate_i_v_t_protection(controller);
#endif

}

void abc( float theta, float d, float q, float *a, float *b, float *c){
    /* Inverse DQ0 Transform
    Phase current amplitude = lengh of dq vector
    i.e. iq = 1, id = 0, peak phase current of 1 */

    float cf = cos_lut(theta);
    float sf = sin_lut(theta);

    *a = cf*d - sf*q;
    *b = (SQRT3_2*sf-.5f*cf)*d - (-SQRT3_2*cf-.5f*sf)*q;
    *c = (-SQRT3_2*sf-.5f*cf)*d - (SQRT3_2*cf-.5f*sf)*q;
    }


void dq0(float theta, float a, float b, float c, float *d, float *q){
    /* DQ0 Transform
    Phase current amplitude = lengh of dq vector
    i.e. iq = 1, id = 0, peak phase current of 1*/

    float cf = cos_lut(theta);
    float sf = sin_lut(theta);

    *d = 0.6666667f*(cf*a + (SQRT3_2*sf-.5f*cf)*b + (-SQRT3_2*sf-.5f*cf)*c);   ///Faster DQ0 Transform
    *q = 0.6666667f*(-sf*a - (-SQRT3_2*cf-.5f*sf)*b - (SQRT3_2*cf-.5f*sf)*c);

    }

void svm(float v_max, float u, float v, float w, float *dtc_u, float *dtc_v, float *dtc_w){
    /* Space Vector Modulation
     u,v,w amplitude = v_bus for full modulation depth */

    float v_offset = (fminf3(u, v, w) + fmaxf3(u, v, w))*0.5f;
    float v_midpoint = .5f*(DTC_MAX+DTC_MIN);

    *dtc_u = fast_fminf(fast_fmaxf((.5f*(u -v_offset)*OVERMODULATION/v_max + v_midpoint ), DTC_MIN), DTC_MAX);
    *dtc_v = fast_fminf(fast_fmaxf((.5f*(v -v_offset)*OVERMODULATION/v_max + v_midpoint ), DTC_MIN), DTC_MAX);
    *dtc_w = fast_fminf(fast_fmaxf((.5f*(w -v_offset)*OVERMODULATION/v_max + v_midpoint ), DTC_MIN), DTC_MAX);

    }

void zero_current(ControllerStruct *controller){
	/* Measure zero-current ADC offset */

#ifdef ADC_SYNC_TRIGGER
	/* The timer-driven conversion has not necessarily occurred while this
	 * startup helper is executing. The enable sequence replaces this with a
	 * completed midpoint sample in zero_current_live() before MOTOR_MODE. */
	(void)controller;
	return;
#else

#ifdef STM32F446
    int adc_a_offset = 0;
    int adc_b_offset = 0;
#else
    int adc_b_offset = 0;
    int adc_c_offset = 0;
#endif
    int n = 1000;
    /* Sample the CSA bias with all three half-bridges at the electrical
     * midpoint.  Sampling at 0% is incorrect when INVERT_DTC is enabled: it
     * drives the complementary output to the rail and changes the shunt
     * amplifier common-mode level, producing a large fake current at idle. */
    controller->dtc_u = 0.5f;
    controller->dtc_v = 0.5f;
    controller->dtc_w = 0.5f;
    set_dtc(controller);

    for (int i = 0; i<n; i++){               // Average n samples
    	analog_sample(controller);
#ifdef STM32F446
    	adc_a_offset += controller->adc_a_raw;
    	adc_b_offset += controller->adc_b_raw;
#else
    	adc_b_offset += controller->adc_b_raw;
        adc_c_offset += controller->adc_c_raw;
#endif
     }
#ifdef STM32F446
    controller->adc_a_offset = adc_a_offset/n;
    controller->adc_b_offset = adc_b_offset/n;
#else
    controller->adc_b_offset = adc_b_offset/n;
    controller->adc_c_offset = adc_c_offset/n;
#endif

#endif

    }

void zero_current_live(ControllerStruct *controller){
	/* The CSA output bias is not guaranteed to be identical with POEN/CHxEN
	 * asserted. This routine is called only after the neutral PWM vector has
	 * been enabled and the DRV charge pump has settled. Discard conversions
	 * already queued before the bridge transition, then average a short window
	 * without changing PWM state. */
#ifdef ADC_SYNC_TRIGGER
	/* The midpoint trigger is asynchronous to this ISR. Start a bounded
	 * accumulator and let subsequent UPDATE samples complete it; waiting here
	 * would consume future PWM events while the ISR is blocked. */
	/* A synchronized conversion may not have completed in the exact service
	 * cycle that reaches this point. Start anyway; the accumulator only counts
	 * subsequent valid UPDATE samples and the ready gate remains closed until
	 * all 64 samples have been committed. */
	if (live_offset_active != 0U) return;
	live_offset_sum_b = 0U;
	live_offset_sum_c = 0U;
	live_offset_count = 0U;
	live_offset_complete = 0U;
	live_offset_avg_b = (uint16_t)controller->adc_b_offset;
	live_offset_avg_c = (uint16_t)controller->adc_c_offset;
	live_offset_active = 1U;
	return;
#else
#ifdef STM32F446
	int adc_a_offset = 0;
	int adc_b_offset = 0;
#else
	int adc_b_offset = 0;
	int adc_c_offset = 0;
#endif
	const int discard = 8;
	const int samples = 64;
#ifndef STM32F446
	adc_flag_clear(ADC_CH_MAIN, ADC_FLAG_EOIC);
	adc_flag_clear(ADC_CH_VBUS, ADC_FLAG_EOIC);
#endif
	for (int i = 0; i < discard + samples; ++i) {
		analog_sample(controller);
		if (controller->adc_valid == 0U) {
			/* Never replace a known-good offset with a stale conversion after
			 * an ADC timeout; the safety path has already disabled the bridge. */
			return;
		}
		if (i < discard) continue;
#ifdef STM32F446
		adc_a_offset += controller->adc_a_raw;
		adc_b_offset += controller->adc_b_raw;
#else
		adc_b_offset += controller->adc_b_raw;
		adc_c_offset += controller->adc_c_raw;
#endif
	}
#ifdef STM32F446
	controller->adc_a_offset = adc_a_offset / samples;
	controller->adc_b_offset = adc_b_offset / samples;
#else
	controller->adc_b_offset = adc_b_offset / samples;
	controller->adc_c_offset = adc_c_offset / samples;
#endif
#endif
}

void init_controller_params(ControllerStruct *controller){

	controller->ki_d = KI_D;
    controller->ki_q = KI_Q;
    controller->k_d = K_SCALE*I_BW;
    controller->k_q = K_SCALE*I_BW;
    controller->alpha = 1.0f - 1.0f/(1.0f - DT*I_BW*TWO_PI_F);
    controller->ki_fw = .1f*controller->ki_d;
    controller->phase_order = PHASE_ORDER;
    if(I_MAX <= 40.0f){controller->i_scale = I_SCALE;}
    else{controller->i_scale = 2.0f*I_SCALE;}
    for(int i = 0; i<128; i++)	// Approximate duty cycle linearization
    {
        controller->inverter_tab[i] = 1.0f + 1.2f*exp(-0.0078125f*i/.032f);
    }

    }

void reset_foc(ControllerStruct *controller){

#ifdef STM32F446
	TIM_PWM.Instance->CCR3 = ((TIM_PWM.Instance->ARR))*(0.5f);
	TIM_PWM.Instance->CCR1 = ((TIM_PWM.Instance->ARR))*(0.5f);
	TIM_PWM.Instance->CCR2 = ((TIM_PWM.Instance->ARR))*(0.5f);
#else
    timer_channel_output_pulse_value_config(TIMER0, TIM_CH_U, SVPWM_PERIOD * 0.5f);
	timer_channel_output_pulse_value_config(TIMER0, TIM_CH_V, SVPWM_PERIOD * 0.5f);
	timer_channel_output_pulse_value_config(TIMER0, TIM_CH_W, SVPWM_PERIOD * 0.5f);
#endif
    controller->i_d_des = 0;
    controller->i_q_des = 0;
    controller->i_d_des_filt = 0;
    controller->i_q_des_filt = 0;
    controller->i_d = 0;
    controller->i_q = 0;
    controller->i_a = 0;
    controller->i_b = 0;
    controller->i_c = 0;
    controller->i_q_filt = 0;
    controller->q_int = 0;
    controller->d_int = 0;
    controller->v_q = 0;
    controller->v_d = 0;
    controller->fw_int = 0;
    controller->otw_flag = 0;
    controller->torque_ramp_cycles = 0U;

    }

void reset_observer(ObserverStruct *observer){
/*
    observer->temperature = 25.0f;
    observer->temp_measured = 25.0f;
    //observer->resistance = .1f;
*/
}

void update_observer(ControllerStruct *controller, ObserverStruct *observer)
{
	/*
    /// Update observer estimates ///
    // Resistance observer //
    // Temperature Observer //
    observer->delta_t = (float)observer->temperature - T_AMBIENT;
    float i_sq = controller->i_d*controller->i_d + controller->i_q*controller->i_q;
    observer->q_in = (R_NOMINAL*1.5f)*(1.0f + .00393f*observer->delta_t)*i_sq;
    observer->q_out = observer->delta_t*R_TH;
    observer->temperature += (INV_M_TH*DT)*(observer->q_in-observer->q_out);

    //float r_d = (controller->v_d*(DTC_MAX-DTC_MIN) + SQRT3*controller->dtheta_elec*(L_Q*controller->i_q))/(controller->i_d*SQRT3);
    float r_q = (controller->v_q*(DTC_MAX-DTC_MIN) - SQRT3*controller->dtheta_elec*(L_D*controller->i_d + WB))/(controller->i_q*SQRT3);
    observer->resistance = r_q;//(r_d*controller->i_d + r_q*controller->i_q)/(controller->i_d + controller->i_q); // voltages more accurate at higher duty cycles

    //observer->resistance = controller->v_q/controller->i_q;
    if(isnan(observer->resistance) || isinf(observer->resistance)){observer->resistance = R_NOMINAL;}
    float t_raw = ((T_AMBIENT + ((observer->resistance/R_NOMINAL) - 1.0f)*254.5f));
    if(t_raw > 200.0f){t_raw = 200.0f;}
    else if(t_raw < 0.0f){t_raw = 0.0f;}
    observer->temp_measured = .999f*observer->temp_measured + .001f*t_raw;
    float e = (float)observer->temperature - observer->temp_measured;
    observer->trust = (1.0f - .004f*fminf(abs(controller->dtheta_elec), 250.0f)) * (.01f*(fminf(i_sq, 100.0f)));
    observer->temperature -= observer->trust*.0001f*e;
    //printf("%.3f\n\r", e);

    if(observer->temperature > TEMP_MAX){controller->otw_flag = 1;}
    else{controller->otw_flag = 0;}
    */
}

float linearize_dtc(ControllerStruct *controller, float dtc)
{
    float duty = fast_fmaxf(fast_fminf(fabs(dtc), .999f), 0.0f);;
    int index = (int) (duty*127.0f);
    float val1 = controller->inverter_tab[index];
    float val2 = controller->inverter_tab[index+1];
    return val1 + (val2 - val1)*(duty*128.0f - (float)index);
}

void field_weaken(ControllerStruct *controller)
{
       /// Field Weakening ///

       controller->fw_int += controller->ki_fw*(controller->v_max - controller->v_ref);
       controller->fw_int = fast_fmaxf(fast_fminf(controller->fw_int, 0.0f), -I_FW_MAX);
       controller->i_d_des = controller->fw_int;
       float q_max = sqrtf(controller->i_max*controller->i_max - controller->i_d_des*controller->i_d_des);
       controller->i_q_des = fast_fmaxf(fast_fminf(controller->i_q_des, q_max), -q_max);


}

static float slew_current_reference(float previous, float target)
{
    const float delta = target - previous;
    if (delta > CURRENT_REF_SLEW_A_PER_CYCLE) {
        return previous + CURRENT_REF_SLEW_A_PER_CYCLE;
    }
    if (delta < -CURRENT_REF_SLEW_A_PER_CYCLE) {
        return previous - CURRENT_REF_SLEW_A_PER_CYCLE;
    }
    return target;
}

void commutate(ControllerStruct *controller, EncoderStruct *encoder)
{
	/* Do Field Oriented Control */

		controller->theta_elec = encoder->elec_angle;
		controller->dtheta_elec = encoder->elec_velocity;
		/* MIT is the motor-side protocol boundary. The AS5047 directly measures
		 * this shaft, so reducer kinematics belong to the upper controller. */
		controller->dtheta_mech = encoder->velocity;
		controller->theta_mech = encoder->angle_multiturn[0];

       /// Commutation  ///
       dq0(controller->theta_elec, controller->i_a, controller->i_b, controller->i_c, &controller->i_d, &controller->i_q);    //dq0 transform on currents - 3.8 us

       controller->i_q_filt = (1.0f-CURRENT_FILT_ALPHA)*controller->i_q_filt + CURRENT_FILT_ALPHA*controller->i_q;	// these aren't used for control but are sometimes nice for debugging
       controller->i_d_filt = (1.0f-CURRENT_FILT_ALPHA)*controller->i_d_filt + CURRENT_FILT_ALPHA*controller->i_d;
       controller->v_bus_filt = (1.0f-VBUS_FILT_ALPHA)*controller->v_bus_filt + VBUS_FILT_ALPHA*controller->v_bus;	// used for voltage saturation

       controller->v_max = OVERMODULATION*controller->v_bus_filt*(DTC_MAX-DTC_MIN)*SQRT1_3;
       controller->i_max = I_MAX; //I_MAX*(!controller->otw_flag) + I_MAX_CONT*controller->otw_flag;
	if (adc_baseline_active != 0U) {
		controller->i_d_des = controller->i_q_des = 0.0f;
		controller->v_d = controller->v_q = 0.0f;
		controller->dtc_u = controller->dtc_v = controller->dtc_w = 0.5f;
		set_dtc(controller);
		return;
	}

#ifndef STM32F446
       if (current_test_active != 0U) {
           /* Hold zero for 300 cycles, then apply one d/q-axis current step.
            * A d-axis step does not intentionally generate torque, so it is
            * the primary PI-tuning signal on a free, unloaded rotor. */
           controller->i_d_des = current_test_axis == 0U && current_test_tick >= 300U ? current_test_step : 0.0f;
           controller->i_q_des = current_test_axis != 0U && current_test_tick >= 300U ? current_test_step : 0.0f;
           if (current_test_tick == 0U) {
               current_test_start_raw_b = (int16_t)controller->adc_b_raw;
               current_test_start_raw_c = (int16_t)controller->adc_c_raw;
               current_test_start_i_b = (int16_t)(controller->i_b * 1000.0f);
               current_test_start_i_c = (int16_t)(controller->i_c * 1000.0f);
               current_test_start_i_d = (int16_t)(controller->i_d * 1000.0f);
               current_test_start_i_q = (int16_t)(controller->i_q * 1000.0f);
               current_test_start_theta_mech = controller->theta_mech;
               current_test_start_theta_elec = controller->theta_elec;
           }
           if (current_test_tick == 300U) {
               current_test_step_i_b = (int16_t)(controller->i_b * 1000.0f);
               current_test_step_i_c = (int16_t)(controller->i_c * 1000.0f);
               current_test_step_i_d = (int16_t)(controller->i_d * 1000.0f);
               current_test_step_i_q = (int16_t)(controller->i_q * 1000.0f);
           }
           if (current_test_tick >= 300U) {
               const float measured = current_test_axis == 0U ? controller->i_d : controller->i_q;
               const float other = current_test_axis == 0U ? controller->i_q : controller->i_d;
               if (current_test_seen == 0U) {
                   current_test_peak = measured;
                   current_test_min = measured;
                   current_test_other_peak = other;
                   current_test_other_min = other;
                   current_test_peak_tick = current_test_tick;
                   current_test_min_tick = current_test_tick;
                   current_test_peak_raw_b = (int16_t)controller->adc_b_raw;
                   current_test_peak_raw_c = (int16_t)controller->adc_c_raw;
                   current_test_peak_i_d = (int16_t)(controller->i_d * 1000.0f);
                   current_test_peak_i_q = (int16_t)(controller->i_q * 1000.0f);
                   current_test_peak_v_d = (int16_t)(controller->v_d * 1000.0f);
                   current_test_peak_v_q = (int16_t)(controller->v_q * 1000.0f);
                   current_test_peak_dtc_u = (int16_t)(controller->dtc_u * 10000.0f);
                   current_test_peak_dtc_v = (int16_t)(controller->dtc_v * 10000.0f);
                   current_test_peak_dtc_w = (int16_t)(controller->dtc_w * 10000.0f);
                   current_test_min_raw_b = current_test_peak_raw_b;
                   current_test_min_raw_c = current_test_peak_raw_c;
                   current_test_min_i_d = current_test_peak_i_d;
                   current_test_min_i_q = current_test_peak_i_q;
                   current_test_min_v_d = current_test_peak_v_d;
                   current_test_min_v_q = current_test_peak_v_q;
                   current_test_min_dtc_u = current_test_peak_dtc_u;
                   current_test_min_dtc_v = current_test_peak_dtc_v;
                   current_test_min_dtc_w = current_test_peak_dtc_w;
                   current_test_seen = 1U;
               } else {
                   if (measured > current_test_peak) {
                       current_test_peak = measured;
                       current_test_peak_tick = current_test_tick;
                       current_test_peak_raw_b = (int16_t)controller->adc_b_raw;
                       current_test_peak_raw_c = (int16_t)controller->adc_c_raw;
                       current_test_peak_i_d = (int16_t)(controller->i_d * 1000.0f);
                       current_test_peak_i_q = (int16_t)(controller->i_q * 1000.0f);
                       current_test_peak_v_d = (int16_t)(controller->v_d * 1000.0f);
                       current_test_peak_v_q = (int16_t)(controller->v_q * 1000.0f);
                       current_test_peak_dtc_u = (int16_t)(controller->dtc_u * 10000.0f);
                       current_test_peak_dtc_v = (int16_t)(controller->dtc_v * 10000.0f);
                       current_test_peak_dtc_w = (int16_t)(controller->dtc_w * 10000.0f);
                   }
                   if (measured < current_test_min) {
                       current_test_min = measured;
                       current_test_min_tick = current_test_tick;
                       current_test_min_raw_b = (int16_t)controller->adc_b_raw;
                       current_test_min_raw_c = (int16_t)controller->adc_c_raw;
                       current_test_min_i_d = (int16_t)(controller->i_d * 1000.0f);
                       current_test_min_i_q = (int16_t)(controller->i_q * 1000.0f);
                       current_test_min_v_d = (int16_t)(controller->v_d * 1000.0f);
                       current_test_min_v_q = (int16_t)(controller->v_q * 1000.0f);
                       current_test_min_dtc_u = (int16_t)(controller->dtc_u * 10000.0f);
                       current_test_min_dtc_v = (int16_t)(controller->dtc_v * 10000.0f);
                       current_test_min_dtc_w = (int16_t)(controller->dtc_w * 10000.0f);
                   }
                   if (other > current_test_other_peak) current_test_other_peak = other;
                   if (other < current_test_other_min) current_test_other_min = other;
               }
               if (current_test_tick >= 5900U) {
                   current_test_final = measured;
                   current_test_final_raw_b = (int16_t)controller->adc_b_raw;
                   current_test_final_raw_c = (int16_t)controller->adc_c_raw;
                   current_test_final_i_b = (int16_t)(controller->i_b * 1000.0f);
                   current_test_final_i_c = (int16_t)(controller->i_c * 1000.0f);
                   current_test_final_i_d = (int16_t)(controller->i_d * 1000.0f);
                   current_test_final_i_q = (int16_t)(controller->i_q * 1000.0f);
                   current_test_final_dtheta_elec = (int16_t)(controller->dtheta_elec * 10.0f);
                   current_test_final_v_max = (int16_t)(controller->v_max * 1000.0f);
                   current_test_final_v_d = (int16_t)(controller->v_d * 1000.0f);
                   current_test_final_v_q = (int16_t)(controller->v_q * 1000.0f);
                   current_test_final_d_int = (int16_t)(controller->d_int * 1000.0f);
                   current_test_final_q_int = (int16_t)(controller->q_int * 1000.0f);
                   current_test_final_v_ref = (int16_t)(controller->v_ref * 1000.0f);
                   current_test_final_theta_mech = controller->theta_mech;
                   current_test_final_theta_elec = controller->theta_elec;
               }
           }
           if (current_test_tick >= CURRENT_TEST_EDGE_START_TICK &&
               ((current_test_tick - CURRENT_TEST_EDGE_START_TICK) % CURRENT_TEST_EDGE_STRIDE) == 0U &&
               ((current_test_tick - CURRENT_TEST_EDGE_START_TICK) / CURRENT_TEST_EDGE_STRIDE) < CURRENT_TEST_EDGE_SAMPLES) {
               const uint8_t index = (uint8_t)((current_test_tick - CURRENT_TEST_EDGE_START_TICK) /
                                               CURRENT_TEST_EDGE_STRIDE);
               current_test_edge_d[index] = (int16_t)(controller->i_d * 1000.0f);
               current_test_edge_q[index] = (int16_t)(controller->i_q * 1000.0f);
               current_test_edge_raw_b[index] = (uint16_t)controller->adc_b_raw;
               current_test_edge_raw_c[index] = (uint16_t)controller->adc_c_raw;
           }
           current_test_tick++;
           if (current_test_tick >= 6000U) {
               controller->i_d_des = 0.0f;
               controller->i_q_des = 0.0f;
               current_test_active = 0U;
           }
       }
#endif

       /* A CAN MIT frame can change t_ff/Kp abruptly.  Slew both current
        * references before the PI loop so a valid command cannot create a
        * sub-cycle phase-current spike that trips DRV8323 OCP. */
       /* Slew from the previous command, not the measured current.  Using
        * i_d/i_q here makes the limiter chase measurement noise and can
        * repeatedly pull a valid MIT reference back toward zero. */
       controller->i_d_des_filt = slew_current_reference(controller->i_d_des_filt,
                                                         controller->i_d_des);
       controller->i_q_des_filt = slew_current_reference(controller->i_q_des_filt,
                                                         controller->i_q_des);
       controller->i_d_des = controller->i_d_des_filt;
       controller->i_q_des = controller->i_q_des_filt;
       limit_norm(&controller->i_d_des, &controller->i_q_des, controller->i_max);	// 2.3 us

       /// PI Controller ///
       float i_d_error = controller->i_d_des - controller->i_d;
       float i_q_error = controller->i_q_des - controller->i_q;


       // Calculate decoupling feed-forward voltages //
       float v_d_ff = 0.0f;//-controller->dtheta_elec*L_Q*controller->i_q;
       float v_q_ff = 0.0f;//controller->dtheta_elec*L_D*controller->i_d;

       controller->v_d = controller->k_d*i_d_error + controller->d_int + v_d_ff;

       controller->v_d = fast_fmaxf(fast_fminf(controller->v_d, controller->v_max), -controller->v_max);

       controller->d_int += controller->k_d*controller->ki_d*i_d_error;
       controller->d_int = fast_fmaxf(fast_fminf(controller->d_int, controller->v_max), -controller->v_max);
       float vq_max = sqrtf(controller->v_max*controller->v_max - controller->v_d*controller->v_d);

       controller->v_q = controller->k_q*i_q_error + controller->q_int + v_q_ff;
       controller->q_int += controller->k_q*controller->ki_q*i_q_error;
       controller->q_int = fast_fmaxf(fast_fminf(controller->q_int, controller->v_max), -controller->v_max);
       controller->v_ref = sqrtf(controller->v_d*controller->v_d + controller->v_q*controller->v_q);
       controller->v_q = fast_fmaxf(fast_fminf(controller->v_q, vq_max), -vq_max);

       limit_norm(&controller->v_d, &controller->v_q, controller->v_max);

       abc(controller->theta_elec + 1.5f*DT*controller->dtheta_elec, controller->v_d, controller->v_q, &controller->v_u, &controller->v_v, &controller->v_w); //inverse dq0 transform on voltages
       svm(controller->v_max, controller->v_u, controller->v_v, controller->v_w, &controller->dtc_u, &controller->dtc_v, &controller->dtc_w); //space vector modulation

       set_dtc(controller);

    }


void torque_control(ControllerStruct *controller){
	/* The host owns position trajectory generation and sends the instantaneous
	 * p/v target. Do not clamp position error here: doing so silently turns a
	 * large commanded move into a fixed low-torque mode. Current-reference slew
	 * and I_MAX remain the firmware safety boundaries. */
	const float position_error = controller->p_des - controller->theta_mech;
	const float startup_current = 5.0f;
	const uint32_t ramp_cycles = 9000U; /* 300 ms at the 30 kHz control loop */
	const float ramp = controller->torque_ramp_cycles >= ramp_cycles ? 1.0f :
		(float)controller->torque_ramp_cycles / (float)ramp_cycles;
	const float ramped_limit = startup_current +
		(controller->i_max - startup_current) * ramp;
	float torque_des = controller->kp * position_error + controller->t_ff +
		controller->kd * (controller->v_des - controller->dtheta_mech);
	controller->i_q_des = fast_fmaxf(fast_fminf(torque_des/KT, ramped_limit), -ramped_limit);
	controller->i_d_des = 0.0f;
	if (controller->torque_ramp_cycles < ramp_cycles) {
		controller->torque_ramp_cycles++;
	}

    }



void zero_commands(ControllerStruct * controller){
	#ifndef STM32F446
	current_test_active = 0U;
	#endif
	controller->t_ff = 0;
	controller->kp = 0;
	controller->kd = 0;
	controller->p_des = 0;
	controller->v_des = 0;
	controller->i_q_des = 0;
	controller->torque_ramp_cycles = 0U;
}
