#include "diagnostics.h"

#ifndef STM32F446

#include <stdio.h>
#include <string.h>
#include "adc.h"
#include "can.h"
#include "config_store.h"
#include "diag_protocol.h"
#include "drv8323.h"
#include "fsm.h"
#include "gpio.h"
#include "hw_config.h"
#include "safety.h"
#include "structs.h"
#include "systick.h"
#include "tim.h"
#include "usart.h"
#include "user_config.h"
#ifdef BRINGUP_INJECT
#include "inject.h"
#endif

DiagnosticCounters diagnostic_counters;
static volatile uint8_t drv_wake_pending;
static volatile uint8_t drv_wake_sequence;
static volatile uint32_t drv_wake_started_ms;

static volatile DiagnosticDebugEntry debug_log[DIAGNOSTIC_DEBUG_LOG_CAPACITY];
static volatile uint8_t debug_enabled_flag;
static volatile uint8_t debug_write_index;
static volatile uint8_t debug_count;
static volatile uint32_t debug_dropped;

/* CAN configuration transactions use four byte little-endian chunks.  The
 * wire protocol only has an 8-bit argument, so writes are staged in RAM and
 * committed atomically after the complete value set passes config validation. */
static int config_stage_int[CONFIG_INT_WORDS];
static float config_stage_float[CONFIG_FLOAT_WORDS];
static uint8_t config_stage_ready;
static uint8_t config_stage_dirty;

typedef struct {
    uint8_t is_float;
    uint8_t index;
} ConfigField;

/* Stable CAN field IDs.  Do not derive these IDs from the backing array
 * indexes: the legacy preference layout has unused/reserved slots. */
static const ConfigField config_fields[] = {
    {0U, 0U}, /* 0 PHASE_ORDER */
    {0U, 1U}, /* 1 CAN_ID */
    {0U, 2U}, /* 2 CAN_MASTER */
    {0U, 3U}, /* 3 CAN_TIMEOUT */
    {0U, 4U}, /* 4 M_ZERO */
    {0U, 5U}, /* 5 E_ZERO */
    {1U, 2U}, /* 6 I_BW */
    {1U, 3U}, /* 7 I_MAX */
    {1U, 9U}, /* 8 I_MAX_CONT */
    {1U, 6U}, /* 9 I_FW_MAX */
    {1U, 18U}, /* 10 I_CAL */
    {1U, 10U}, /* 11 PPAIRS */
    {1U, 14U}, /* 12 KT */
    {1U, 17U}, /* 13 GR */
    {1U, 19U}, /* 14 P_MIN */
    {1U, 20U}, /* 15 P_MAX */
    {1U, 21U}, /* 16 V_MIN */
    {1U, 22U}, /* 17 V_MAX */
    {1U, 23U}, /* 18 KP_MAX */
    {1U, 24U}, /* 19 KD_MAX */
    {1U, 8U}, /* 20 TEMP_MAX */
    {0U, 7U}, /* 21 IVT_PROTECT_ENABLE */
    {1U, 25U}, /* 22 I_TRIP */
    {1U, 26U}, /* 23 VBUS_MIN */
    {1U, 27U}, /* 24 VBUS_MAX */
    {1U, 28U}  /* 25 TEMP_TRIP */
};
#define CONFIG_FIELD_COUNT ((uint8_t)(sizeof(config_fields) / sizeof(config_fields[0])))

static void config_stage_begin(void)
{
    if (config_stage_ready == 0U) {
        memcpy(config_stage_int, __int_reg, sizeof(config_stage_int));
        memcpy(config_stage_float, __float_reg, sizeof(config_stage_float));
        config_stage_ready = 1U;
    }
}

static uint8_t *config_stage_field(uint8_t field, uint32_t *size)
{
    config_stage_begin();
    if (field >= CONFIG_FIELD_COUNT) {
        *size = 0U;
        return NULL;
    }
    if (config_fields[field].is_float != 0U) {
        *size = sizeof(float);
        return (uint8_t *)&config_stage_float[config_fields[field].index];
    }
    *size = sizeof(int);
    return (uint8_t *)&config_stage_int[config_fields[field].index];
}

static uint32_t config_control_request(const DiagRequest *request, uint8_t *status)
{
    uint8_t field;
    uint8_t offset;
    uint32_t size;
    uint8_t *bytes;

    *status = DIAG_STATUS_OK;
    if (request->page >= 0x20U &&
        request->page < (uint8_t)(0x20U + CONFIG_FIELD_COUNT * 4U)) {
        uint8_t relative = (uint8_t)(request->page - 0x20U);
        field = (uint8_t)(relative / 4U);
        offset = (uint8_t)(relative % 4U);
        bytes = config_stage_field(field, &size);
        if (bytes == NULL || offset >= size) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        bytes[offset] = request->argument;
        config_stage_dirty = 1U;
        return ((uint32_t)field << 8) | offset;
    }
    if (request->page >= 0x90U &&
        request->page < (uint8_t)(0x90U + CONFIG_FIELD_COUNT * 4U)) {
        uint8_t relative = (uint8_t)(request->page - 0x90U);
        field = (uint8_t)(relative / 4U);
        offset = (uint8_t)(relative % 4U);
        bytes = config_stage_field(field, &size);
        if (bytes == NULL || offset >= size) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        return bytes[offset];
    }
    if (request->page == 0xF8U) {
        config_stage_begin();
        if (!config_payload_valid(config_stage_int, config_stage_float)) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        return config_payload_crc32(config_stage_int, config_stage_float);
    }
    if (request->page == 0xF9U) {
        if (state.state != MENU_MODE || state.next_state != MENU_MODE ||
            safety_get_faults() != 0U || drv.fault != 0U || config_stage_dirty == 0U) {
            *status = DIAG_STATUS_BUSY;
            return 0U;
        }
        config_stage_begin();
        if (!config_payload_valid(config_stage_int, config_stage_float)) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        memcpy(__int_reg, config_stage_int, sizeof(config_stage_int));
        memcpy(__float_reg, config_stage_float, sizeof(config_stage_float));
        if (fsm_save_preferences() != 0) {
            *status = DIAG_STATUS_UNAVAILABLE;
            return 0U;
        }
        config_stage_dirty = 0U;
        return 1U;
    }
    if (request->page == 0xFAU) {
        config_stage_ready = 0U;
        config_stage_dirty = 0U;
        return 1U;
    }
    *status = DIAG_STATUS_BAD_PAGE;
    return 0U;
}

void diagnostics_debug_clear(void);

void diagnostics_debug_set(uint8_t enabled)
{
    debug_enabled_flag = enabled != 0U ? 1U : 0U;
    if (debug_enabled_flag != 0U) diagnostics_debug_clear();
}

void diagnostics_debug_clear(void)
{
    debug_write_index = 0U;
    debug_count = 0U;
    debug_dropped = 0U;
}

uint8_t diagnostics_debug_enabled(void)
{
    return debug_enabled_flag;
}

uint32_t diagnostics_debug_status(void)
{
    return (uint32_t)debug_enabled_flag |
           ((uint32_t)debug_count << 8) |
           ((debug_dropped > 0xFFFFU ? 0xFFFFU : debug_dropped) << 16);
}

void diagnostics_debug_record(uint8_t event, uint32_t payload)
{
    uint8_t index;
    if (debug_enabled_flag == 0U) return;
    index = debug_write_index;
    debug_log[index].timestamp_ms = systick_uptime_ms();
    debug_log[index].event = event;
    debug_log[index].payload = payload;
    debug_write_index = (uint8_t)((index + 1U) % DIAGNOSTIC_DEBUG_LOG_CAPACITY);
    if (debug_count < DIAGNOSTIC_DEBUG_LOG_CAPACITY) debug_count++;
    else debug_dropped++;
}

int diagnostics_debug_read(uint8_t index, uint8_t field, uint32_t *payload)
{
    uint8_t oldest;
    uint8_t slot;
    if (payload == NULL || index >= debug_count || field > 2U) return -1;
    oldest = (uint8_t)((debug_write_index + DIAGNOSTIC_DEBUG_LOG_CAPACITY - debug_count) %
                       DIAGNOSTIC_DEBUG_LOG_CAPACITY);
    slot = (uint8_t)((oldest + index) % DIAGNOSTIC_DEBUG_LOG_CAPACITY);
    if (field == 0U) *payload = debug_log[slot].timestamp_ms;
    else if (field == 1U) *payload = debug_log[slot].event;
    else *payload = debug_log[slot].payload;
    return 0;
}

static uint32_t milli_payload(float value)
{
    int32_t scaled;

    if (value >= 2147483.0f) return 0x7FFFFFFFU;
    if (value <= -2147483.0f) return 0x80000000U;
    scaled = (int32_t)(value * 1000.0f);
    return (uint32_t)scaled;
}

static int16_t current_milli16(float value)
{
    if (value >= 32.767f) return 32767;
    if (value <= -32.768f) return -32768;
    return (int16_t)(value * 1000.0f);
}

static int16_t adc_delta16(int raw, int offset)
{
    int delta = raw - offset;
    if (delta > 32767) return 32767;
    if (delta < -32768) return -32768;
    return (int16_t)delta;
}

static uint32_t runtime_gate_flags(void)
{
    uint32_t flags = 0U;

    flags |= drv_enable_ready() != 0 ? (1U << 0) : 0U;
    flags |= drv.fault != 0U ? (1U << 1) : 0U;
    flags |= controller.adc_valid != 0U ? (1U << 2) : 0U;
    flags |= comm_encoder.valid != 0U ? (1U << 3) : 0U;
    flags |= gpio_output_bit_get(GPIOA, GPIO_PIN_11) != RESET ? (1U << 4) : 0U;
    flags |= (TIMER_CCHP(TIMER0) & TIMER_CCHP_POEN) != 0U ? (1U << 5) : 0U;
    flags |= (TIMER_CHCTL2(TIMER0) & TIMER_CHCTL2_CH0EN) != 0U ? (1U << 6) : 0U;
    flags |= (TIMER_CHCTL2(TIMER0) & TIMER_CHCTL2_CH1EN) != 0U ? (1U << 7) : 0U;
    flags |= (TIMER_CHCTL2(TIMER0) & TIMER_CHCTL2_CH2EN) != 0U ? (1U << 8) : 0U;
    return flags;
}

/* This checksum is diagnostic evidence only.  It covers the calibration LUT
 * currently loaded from preferences, without exposing all 128 entries on CAN. */
static uint32_t encoder_lut_checksum(void)
{
    uint32_t checksum = 2166136261U;

    for (uint32_t i = 0U; i < 128U; ++i) {
        checksum ^= (uint32_t)((const int *)&ENCODER_LUT)[i];
        checksum *= 16777619U;
    }
    return checksum;
}

static uint32_t diagnostic_payload(const DiagRequest *request, uint8_t *status)
{
    *status = DIAG_STATUS_OK;

    switch (request->opcode) {
    case DIAG_OPCODE_PING:
        if (request->page == 0U) {
            return ((uint32_t)'A') | ((uint32_t)'T' << 8) |
                   ((uint32_t)'H' << 16) | ((uint32_t)'N' << 24);
        }
        break;

    case DIAG_OPCODE_GET_INFO:
        switch (request->page) {
        case 0U: return 0x4E485441U; /* "ATHN" little-endian */
        case 1U: return (1U << 16);  /* diagnostic ABI 1.0 */
#ifdef BRINGUP_INJECT
        case 2U: return 0x0001000FU; /* safe, gate-off, no flash, UART TX-only, inject */
        case 5U: return (1U << 24) |
                       ((uint32_t)DIAG_INJECT_DURATION_TABLE_SIZE << 16) |
                       ((uint32_t)DIAG_INJECT_DUTY_TABLE_SIZE << 8) |
                       (uint32_t)DIAG_INJECT_VECTOR_COUNT;
#else
        case 2U: return 0x0000000FU; /* safe, gate-off, no flash, UART TX-only */
#endif
        case 3U: return 0xA7E0D101U; /* stable safe-bring-up fingerprint */
        case 4U: return DIAG_NODE_ID | (1000U << 8); /* node + kbit/s */
        default: break;
        }
        break;

    case DIAG_OPCODE_GET_SNAPSHOT:
        switch (request->page) {
        case 0U: return controller.adc_sample_count;
        case 1U: return systick_uptime_ms();
        case 2U: return safety_get_faults();
        case 3U: {
            uint32_t flags = 0U;
            flags |= (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) ? (1U << 0) : 0U;
            flags |= (gpio_output_bit_get(GPIOA, GPIO_PIN_11) != RESET) ? (1U << 1) : 0U;
            flags |= (TIMER_CCHP(TIMER0) & TIMER_CCHP_POEN) != 0U ? (1U << 2) : 0U;
            flags |= comm_encoder.valid != 0U ? (1U << 3) : 0U;
            flags |= controller.adc_valid != 0U ? (1U << 4) : 0U;
            flags |= (CAN_ERR(CAN0) & CAN_ERR_WERR) != 0U ? (1U << 5) : 0U;
            flags |= (CAN_ERR(CAN0) & CAN_ERR_PERR) != 0U ? (1U << 6) : 0U;
            flags |= (CAN_ERR(CAN0) & CAN_ERR_BOERR) != 0U ? (1U << 7) : 0U;
            flags |= (TIMER_CHCTL2(TIMER0) & TIMER_CHCTL2_CH0EN) != 0U ? (1U << 8) : 0U;
            flags |= (TIMER_CHCTL2(TIMER0) & TIMER_CHCTL2_CH1EN) != 0U ? (1U << 9) : 0U;
            flags |= (TIMER_CHCTL2(TIMER0) & TIMER_CHCTL2_CH2EN) != 0U ? (1U << 10) : 0U;
            flags |= 1U << 31;
            return flags;
        }
        case 4U: return (uint32_t)comm_encoder.last_frame |
                        ((uint32_t)comm_encoder.raw14 << 16);
        case 5U: return (uint32_t)comm_encoder.count;
        case 6U: return (uint32_t)comm_encoder.turns;
        case 9U: return ((uint32_t)(uint16_t)controller.adc_b_raw) |
                        ((uint32_t)(uint16_t)controller.adc_c_raw << 16);
        case 10U: return (uint32_t)(uint16_t)controller.adc_vbus_raw;
        case 11U: return (uint32_t)adc01.data.temp_pcb |
                         ((uint32_t)adc01.data.temp_motor << 16);
        case 12U: return (uint32_t)adc01.data.hall0 |
                         ((uint32_t)adc01.data.hall1 << 16);
        case 13U: return (uint32_t)adc01.data.hall2 |
                         ((uint32_t)adc01.data.hall3 << 16);
        case 14U: return (uint32_t)adc01.data.hall4 |
                         ((uint32_t)adc01.data.hall5 << 16);
        case 15U:
            if (comm_encoder.diagnostics_valid == 0U) {
                *status = DIAG_STATUS_UNAVAILABLE;
                return 0U;
            }
            return (uint32_t)comm_encoder.diaagc |
                   ((uint32_t)comm_encoder.magnitude << 16);
        case 16U: return CAN_ERR(CAN0);
        case 17U: return (uint32_t)controller.loop_count;
        case 18U: return (TIMER_CH0CV(TIMER0) & 0xFFFFU) |
                         ((TIMER_CH1CV(TIMER0) & 0xFFFFU) << 16);
        case 19U: return (TIMER_CH2CV(TIMER0) & 0xFFFFU) |
                         ((TIMER_CAR(TIMER0) & 0xFFFFU) << 16);
#ifndef BRINGUP_INJECT
        /* Keep the tested DRV evidence available in the normal image. */
        case 24U: return drv_init_final_fsr();
        case 25U: return (uint32_t)DRV_DIAG_DCR_VALUE |
                          ((uint32_t)(I_MAX <= 40.0f ? DRV_DIAG_CSACR_VALUE_40A :
                                      DRV_DIAG_CSACR_VALUE_60A) << 16);
        case 26U: return DRV_DIAG_OCPCR_VALUE;
        case 27U: return (uint32_t)drv_enable_ready();
        case 28U: return (uint32_t)DRV_DIAG_DCR_VALUE |
                          ((uint32_t)(I_MAX <= 40.0f ? DRV_DIAG_CSACR_VALUE_40A :
                                      DRV_DIAG_CSACR_VALUE_60A) << 16);
        case 29U: return drv_init_final_fsr();
        case 30U: return DRV_DIAG_OCPCR_VALUE;
        case 56U: return drv_init_reason();
        case 57U: return drv_init_pre_fsr();
        case 58U: return drv_init_final_fsr();
        case 59U: return drv_init_readback_dcr_csacr();
        case 60U: return drv_init_readback_ocpcr();
        case 61U: return drv_init_spi_rx(0U);
        case 62U: return drv_init_spi_rx(1U);
        case 63U: return drv_init_spi_rx(2U);
        case 64U: return drv_init_spi_rx(3U);
        case 65U: return drv_init_spi_rx(4U);
        case 66U: return drv_init_spi_rx(5U);
        case 67U: return drv_enable_evidence(0U);
        case 68U: return drv_enable_evidence(1U);
        case 69U: return drv_enable_evidence(2U);
        case 70U: return drv_enable_evidence(3U);
        case 71U: return drv_enable_evidence(4U);
        case 72U: return drv_enable_evidence(5U);
        case 73U: return drv_enable_evidence(6U);
        case 74U: return drv_enable_evidence(7U);  /* enable GPIOA CTL1 */
        case 75U: return drv_enable_evidence(8U);  /* enable GPIOA OCTL */
        case 76U: return drv_enable_evidence(9U);  /* enable GPIOA ISTAT */
        case 77U: return drv_enable_evidence(10U); /* enable GPIOB CTL1 */
        case 78U: return drv_enable_evidence(11U); /* enable GPIOB OCTL */
        case 79U: return drv_enable_evidence(12U); /* enable GPIOB ISTAT */
        case 80U: return drv_enable_evidence(13U); /* enable SPI STAT */
        case 81U: return drv_enable_evidence(14U); /* nFAULT edge in enable window */
        /* Runtime control evidence. Milli-unit pages are signed int32 payloads. */
        case 82U: return (uint32_t)state.state |
                          ((uint32_t)state.next_state << 8) |
                          ((uint32_t)state.ready << 16);
        case 83U: return runtime_gate_flags();
        case 84U: return milli_payload(I_MAX);
        case 85U: return milli_payload(controller.i_q_des);
        case 86U: return milli_payload(controller.i_q_filt);
        case 87U: return (uint32_t)(uint16_t)current_milli16(controller.i_a) |
                          ((uint32_t)(uint16_t)current_milli16(controller.i_b) << 16);
        case 88U: return (TIMER_CH0CV(TIMER0) & 0xFFFFU) |
                          ((TIMER_CH1CV(TIMER0) & 0xFFFFU) << 16);
        case 89U: return (TIMER_CH2CV(TIMER0) & 0xFFFFU) |
                          ((TIMER_CAR(TIMER0) & 0xFFFFU) << 16);
        case 90U: return milli_payload(controller.p_des);
        case 91U: return milli_payload(controller.v_des);
        case 92U: return milli_payload(controller.kp);
        case 93U: return milli_payload(controller.kd);
        case 94U: return milli_payload(controller.t_ff);
        case 95U: return (uint32_t)state.state |
                          ((uint32_t)state.next_state << 8) |
                          ((uint32_t)comm_encoder_cal.started << 16) |
                          ((uint32_t)comm_encoder_cal.done_ordering << 24) |
                          ((uint32_t)comm_encoder_cal.done_cal << 25) |
                          ((uint32_t)comm_encoder_cal.failed << 26);
        case 96U: return (uint32_t)comm_encoder_cal.phase_order |
                          ((uint32_t)comm_encoder_cal.ppairs << 8) |
                          ((uint32_t)comm_encoder_cal.sample_count << 16);
        case 97U: return (uint32_t)comm_encoder_cal.ezero;
        case 98U: return milli_payload(I_CAL);
        case 99U: return diagnostics_debug_status();
		case 100U: return milli_payload(comm_encoder_cal.theta_start);
		case 101U: return milli_payload(comm_encoder_cal.evidence_theta_end);
		case 102U: return milli_payload(comm_encoder_cal.evidence_angle_delta);
		case 103U: return milli_payload(comm_encoder_cal.evidence_i_d_des);
		case 104U: return milli_payload(comm_encoder_cal.evidence_i_d);
		case 105U: return milli_payload(comm_encoder_cal.evidence_i_q);
		case 106U: return milli_payload(comm_encoder_cal.evidence_v_d);
		case 107U: return milli_payload(comm_encoder_cal.evidence_v_q);
		case 108U: return (uint32_t)(uint16_t)(comm_encoder_cal.evidence_dtc_u * 10000.0f) |
					  ((uint32_t)(uint16_t)(comm_encoder_cal.evidence_dtc_v * 10000.0f) << 16);
		case 109U: return (uint32_t)(uint16_t)(comm_encoder_cal.evidence_dtc_w * 10000.0f);
		case 110U: return milli_payload(comm_encoder_cal.evidence_theta_ref);
		/* Persisted configuration as loaded at boot.  Unlike pages 95-110,
		 * these do not belong to the transient calibration session. */
		case 111U: return milli_payload(PPAIRS);
		case 112U: return (uint32_t)PHASE_ORDER |
		                  ((uint32_t)(uint16_t)comm_encoder.ppairs << 16);
		case 113U: return (uint32_t)E_ZERO;
		case 114U: return config_payload_crc32(__int_reg, __float_reg);
		case 115U: return encoder_lut_checksum();
		/* ADC/current-chain evidence.  These pages are deliberately read-only and
		 * expose the exact values used by analog_sample(), not reconstructed values. */
		case 116U: return (uint32_t)((uint16_t)controller.adc_b_raw) |
		                  ((uint32_t)(uint16_t)controller.adc_c_raw << 16);
		case 117U: return (uint32_t)((uint16_t)controller.adc_b_offset) |
		                  ((uint32_t)(uint16_t)controller.adc_c_offset << 16);
		case 118U: return (uint32_t)(uint16_t)adc_delta16(controller.adc_b_raw,
		                                                   controller.adc_b_offset) |
		                  ((uint32_t)(uint16_t)adc_delta16(controller.adc_c_raw,
		                                                   controller.adc_c_offset) << 16);
		case 119U: return (uint32_t)(controller.i_scale * 1000000.0f);
		case 120U: return (uint32_t)(uint16_t)current_milli16(controller.i_b) |
		                  ((uint32_t)(uint16_t)current_milli16(controller.i_c) << 16);
		case 121U: return (uint32_t)controller.adc_valid |
		                  ((controller.adc_sample_count & 0x00FFFFFFU) << 8);
		case 122U: return (uint32_t)controller.adc_timeout_count;
		/* Captured from the active-low nFAULT ISR before EN_GATE is dropped.
		 * Pages 123-136 retain the last runtime fault until the next fault. */
		case 123U: return drv_runtime_fault_evidence(0U); /* timestamp_ms */
		case 124U: return drv_runtime_fault_evidence(1U); /* FSR1 | FSR2 << 16 */
		case 125U: return drv_runtime_fault_evidence(2U); /* ADC B | ADC C << 16 */
		case 126U: return drv_runtime_fault_evidence(3U); /* B offset | C offset << 16 */
		case 127U: return drv_runtime_fault_evidence(4U); /* VBUS ADC raw */
		case 128U: return drv_runtime_fault_evidence(5U); /* GPIOA ISTAT */
		case 129U: return drv_runtime_fault_evidence(6U); /* GPIOA OCTL */
		case 130U: return drv_runtime_fault_evidence(7U); /* i_q_des mA */
		case 131U: return drv_runtime_fault_evidence(8U); /* i_q mA */
		case 132U: return drv_runtime_fault_evidence(9U); /* i_d mA */
		case 133U: return drv_runtime_fault_evidence(10U); /* i_q_filt mA */
		case 134U: return drv_runtime_fault_evidence(11U); /* vbus_filt mV */
		case 135U: return drv_runtime_fault_evidence(12U); /* duty U | V << 16 */
		case 136U: return drv_runtime_fault_evidence(13U); /* duty W */
		/* I/V/T protection configuration and uncalibrated-source status.  These
		 * pages let the host refuse to enable a threshold before it has captured
		 * the raw ADC evidence needed to calibrate it. */
		case 137U: return (uint32_t)IVT_PROTECT_ENABLE;
		case 138U: return milli_payload(I_TRIP);
		case 139U: return milli_payload(VBUS_MIN);
		case 140U: return milli_payload(VBUS_MAX);
		case 141U: return milli_payload(TEMP_TRIP);
		case 142U: return (uint32_t)(uint16_t)controller.adc_vbus_raw |
		                  ((uint32_t)controller.adc_valid << 16);
#endif
#ifdef BRINGUP_INJECT
        case 20U: /* fallthrough to shared handler */
        case 21U:
        case 22U:
        case 23U:
        case 24U:
        case 25U:
        case 26U:
        case 27U:
        case 28U:
        case 29U:
        case 30U:
        case 31U:
        case 32U:
        case 33U:
        case 34U:
        case 35U:
        case 36U:
        case 37U:
        case 38U:
        case 39U:
        case 40U:
        case 41U:
        case 42U:
        case 43U:
        case 44U:
        case 45U:
        case 46U:
        case 47U:
        case 48U:
        case 49U:
        case 50U:
        case 51U:
        case 52U:
        case 53U:
        case 54U:
        case 55U:
            return inject_snapshot(request->page, status);
#endif
        default: break;
        }
        break;

    case DIAG_OPCODE_GET_COUNTER:
        switch (request->page) {
        case 0U: return diagnostic_counters.rx_total;
        case 1U: return diagnostic_counters.rx_valid;
        case 2U: return diagnostic_counters.rx_bad_format;
        case 3U: return diagnostic_counters.rx_bad_protocol;
        case 4U: return diagnostic_counters.rx_bad_opcode_page;
        case 5U: return diagnostic_counters.rx_rate_drop;
        case 6U: return diagnostic_counters.tx_submit;
        case 7U: return diagnostic_counters.tx_no_mailbox;
        case 9U: return comm_encoder.spi_timeout_count;
        case 10U: return controller.adc_timeout_count;
        case 11U: return comm_encoder.parity_error_count;
        case 12U: return comm_encoder.sensor_error_count;
        case 13U: return comm_encoder.jump_error_count;
        case 15U: return uart_tx_timeout_count;
        default: break;
        }
        break;

    default:
        *status = DIAG_STATUS_UNSUPPORTED;
        return 0U;
    }

    *status = DIAG_STATUS_BAD_PAGE;
    return 0U;
}

static uint32_t handle_control_request(const DiagRequest *request, uint8_t *status)
{
#if defined(SAFE_BRINGUP) || defined(BRINGUP_INJECT)
    (void)request;
    *status = DIAG_STATUS_UNSUPPORTED;
    return 0U;
#else
    uint32_t value;
    /* page is a structured command, not an arbitrary UART byte stream. */
    if ((request->page >= 0x20U &&
         request->page < (uint8_t)(0x20U + CONFIG_FIELD_COUNT * 4U)) ||
        (request->page >= 0x90U &&
         request->page < (uint8_t)(0x90U + CONFIG_FIELD_COUNT * 4U)) ||
        request->page >= 0xF8U) {
        return config_control_request(request, status);
    }
    switch (request->page) {
    case 1U: /* ESC */
        update_fsm(&state, ESC_CMD);
        return 1U;
    case 2U: /* MOTOR */
        update_fsm(&state, MOTOR_CMD);
        return 2U;
    case 3U: /* ENCODER read-only mode */
        update_fsm(&state, ENCODER_CMD);
        return 3U;
    case 4U: /* CALIBRATE; argument is current in 0.1 A units */
        if (request->argument < 1U || request->argument > 20U ||
            state.state != MENU_MODE || state.next_state != MENU_MODE ||
            safety_get_faults() != 0U || drv.fault != 0U ||
            comm_encoder.valid == 0U || controller.adc_valid == 0U ||
            gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
            *status = DIAG_STATUS_BUSY;
            return 0U;
        }
        I_CAL = (float)request->argument * 0.1f;
        update_fsm(&state, CAL_CMD);
        return (uint32_t)request->argument * 100U;
    case 5U: /* ZERO; explicit CAN command, writes the transactional config */
        if (state.state != MENU_MODE || state.next_state != MENU_MODE ||
            safety_get_faults() != 0U || comm_encoder.valid == 0U) {
            *status = DIAG_STATUS_BUSY;
            return 0U;
        }
        update_fsm(&state, ZERO_CMD);
        return 1U;
    case 6U: /* ABORT current state and return to the safe menu */
        if (state.state == MENU_MODE && state.next_state == MENU_MODE) {
            *status = DIAG_STATUS_BUSY;
            return 0U;
        }
        update_fsm(&state, ESC_CMD);
        return 1U;
    case 7U: /* Enable RAM debug log; enabling also starts a fresh log. */
        diagnostics_debug_set(1U);
        return diagnostics_debug_status();
    case 8U: /* Disable RAM debug log. */
        diagnostics_debug_set(0U);
        return diagnostics_debug_status();
    case 9U: /* Clear RAM debug log, preserving enable state. */
        diagnostics_debug_clear();
        return diagnostics_debug_status();
    case 10U: /* Read timestamp for log index, argument selects entry. */
        if (diagnostics_debug_read(request->argument, 0U, &value) != 0) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        return value;
    case 11U: /* Read event code for log index. */
        if (diagnostics_debug_read(request->argument, 1U, &value) != 0) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        return value;
    case 12U: /* Read event payload for log index. */
        if (diagnostics_debug_read(request->argument, 2U, &value) != 0) {
            *status = DIAG_STATUS_BAD_PAGE;
            return 0U;
        }
        return value;
    default:
        *status = DIAG_STATUS_UNSUPPORTED;
        return 0U;
    }
#endif
}

void diagnostics_handle_can(const can_receive_message_struct *message)
{
    static uint32_t last_response_ms;
    DiagRequest request;
    can_trasnmit_message_struct response;
    uint8_t status = DIAG_STATUS_OK;
    uint32_t payload;
    uint32_t now;

    diagnostic_counters.rx_total++;
    if (message->rx_sfid != DIAG_CAN_REQUEST_ID ||
        message->rx_ff != CAN_FF_STANDARD || message->rx_ft != CAN_FT_DATA ||
        message->rx_dlen != 8U) {
        diagnostic_counters.rx_bad_format++;
        return;
    }
    if (diag_protocol_parse(message->rx_data, &request) != 0) {
        diagnostic_counters.rx_bad_protocol++;
        return;
    }

    now = systick_uptime_ms();
    if ((uint32_t)(now - last_response_ms) < 20U) {
        diagnostic_counters.rx_rate_drop++;
        return;
    }
    last_response_ms = now;
    diagnostic_counters.rx_valid++;

    if (request.opcode == DIAG_OPCODE_DRV_WAKE) {
        /* Do not delay or poll SPI in CAN RX. The 30 kHz timer service waits
         * for the charge pump, then performs the two bounded transfers. */
        drv_wake_sequence = request.sequence;
        drv_wake_started_ms = now;
        drv_wake_pending = 1U;
        gpio_bit_set(ENABLE_PIN);
        return;
    } else {
        payload = request.opcode == DIAG_OPCODE_CONTROL
                  ? handle_control_request(&request, &status)
                  : diagnostic_payload(&request, &status);
    }
    if (status == DIAG_STATUS_BAD_PAGE || status == DIAG_STATUS_UNSUPPORTED) {
        diagnostic_counters.rx_bad_opcode_page++;
    }

    can_struct_para_init(CAN_TX_MESSAGE_STRUCT, &response);
    response.tx_sfid = DIAG_CAN_RESPONSE_ID;
    response.tx_efid = 0U;
    response.tx_ft = CAN_FT_DATA;
    response.tx_ff = CAN_FF_STANDARD;
    response.tx_dlen = 8U;
    diag_protocol_response(&request, status, payload, response.tx_data);

    if (can_message_transmit(CAN0, &response) == CAN_NOMAILBOX) {
        diagnostic_counters.tx_no_mailbox++;
    } else {
        diagnostic_counters.tx_submit++;
    }
}

void diagnostics_drv_wake_service(void)
{
    can_trasnmit_message_struct response;
    uint16_t fsr1, fsr2;
    uint8_t sequence;
    if (drv_wake_pending == 0U ||
        (uint32_t)(systick_uptime_ms() - drv_wake_started_ms) < 10U) return;
    sequence = drv_wake_sequence;
    fsr1 = drv_read_FSR1(drv);
    fsr2 = drv_read_FSR2(drv);
    gpio_bit_reset(ENABLE_PIN);
    drv_wake_pending = 0U;
    can_struct_para_init(CAN_TX_MESSAGE_STRUCT, &response);
    response.tx_sfid = DIAG_CAN_RESPONSE_ID;
    response.tx_ft = CAN_FT_DATA; response.tx_ff = CAN_FF_STANDARD;
    response.tx_dlen = 8U;
    {
        DiagRequest request;
        request.opcode = DIAG_OPCODE_DRV_WAKE;
        request.sequence = sequence; request.page = 0U; request.argument = 0U;
        diag_protocol_response(&request, DIAG_STATUS_OK,
                                (uint32_t)fsr1 | ((uint32_t)fsr2 << 16),
                                response.tx_data);
    }
    (void)can_message_transmit(CAN0, &response);
}

uint8_t diagnostics_drv_wake_window_active(void)
{
    /* PA11 is asserted only for this bounded, PWM-off diagnostic transaction.
     * nFAULT must be sampled by the service before normal fault handling drops
     * EN_GATE; otherwise the read would observe the disabled DRV bus. */
    return drv_wake_pending;
}

void diagnostics_uart_report(void)
{
    printf("S1 ms=%lu fault=%08lx gate=%u poe=%u enc_ok=%u raw14=%u frame=%04x dia=%04x mag=%04x canerr=%08lx\r\n",
           systick_uptime_ms(), safety_get_faults(),
           gpio_output_bit_get(GPIOA, GPIO_PIN_11) != RESET,
           (TIMER_CCHP(TIMER0) & TIMER_CCHP_POEN) != 0U,
           comm_encoder.valid, comm_encoder.raw14, comm_encoder.last_frame,
           comm_encoder.diaagc, comm_encoder.magnitude, CAN_ERR(CAN0));
    printf("A1 adc_ok=%u b=%d c=%d vbus=%d tp=%u tm=%u hall=%u,%u,%u,%u,%u,%u\r\n",
           controller.adc_valid, controller.adc_b_raw, controller.adc_c_raw,
           controller.adc_vbus_raw, adc01.data.temp_pcb, adc01.data.temp_motor,
           adc01.data.hall0, adc01.data.hall1, adc01.data.hall2,
           adc01.data.hall3, adc01.data.hall4, adc01.data.hall5);
}

#endif /* !STM32F446 */
