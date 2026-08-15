/*
 * inject.c
 *
 * BRINGUP_INJECT-only profile. Default state is passive: PA11 low and all PWM
 * compare values at the all-low position. A validated CAN request arms a
 * single bounded pulse whose voltage vector, duty cycle and duration are taken
 * only from fixed compile-time tables. The 30 kHz timer ISR performs the real
 * hardware sequencing and enforces deadline, nFAULT, latched-fault and
 * current-deviation aborts on every tick.
 */

#ifdef BRINGUP_INJECT

#include "inject.h"

#include "adc.h"
#include "diag_protocol.h"
#include "diagnostics.h"
#include "drv8323.h"
#include "foc.h"
#include "gpio.h"
#include "hw_config.h"
#include "position_sensor.h"
#include "safety.h"
#include "structs.h"
#include "systick.h"
#include "tim.h"

/* High-side duty in per-mille: 0.5% .. 5.0%. The supply current limit, not
 * firmware, remains the primary over-current protection; this table is the
 * maximum the operator is ever allowed to request. */
#define INJECT_DUTY_TABLE_SIZE DIAG_INJECT_DUTY_TABLE_SIZE
static const uint16_t inject_duty_permille[INJECT_DUTY_TABLE_SIZE] = {
    5U, 10U, 15U, 20U, 25U, 30U, 35U, 40U, 45U, 50U
};

/* Pulse duration in milliseconds. */
#define INJECT_DURATION_TABLE_SIZE DIAG_INJECT_DURATION_TABLE_SIZE
static const uint16_t inject_duration_ms[INJECT_DURATION_TABLE_SIZE] = {
    10U, 20U, 30U, 40U, 50U
};

/* Which channels are at the high-side duty for each of the six BLDC step
 * vectors. All other channels sit at the all-low (INH low) position. */
static const uint8_t inject_vector_high[DIAG_INJECT_VECTOR_COUNT][3] = {
    {1U, 0U, 0U}, /* U+     */
    {1U, 1U, 0U}, /* U+ V+  */
    {0U, 1U, 0U}, /* V+     */
    {0U, 1U, 1U}, /* V+ W+  */
    {0U, 0U, 1U}, /* W+     */
    {1U, 0U, 1U}  /* W+ U+  */
};

#define INJECT_MAX_DURATION_MS 50U
#define INJECT_TICKS_PER_MS 30U
#define INJECT_COOLDOWN_MS 500U

/* ADC deviation ceiling in counts. At the unverified I_SCALE hypothesis this
 * is roughly 2 A, far above the 0.2 A bench limit but still a fast way to
 * catch gross phase/short faults before the supply reacts. */
#define INJECT_ADC_DEVIATION_LIMIT 100

typedef enum {
    INJECT_STATE_IDLE = 0U,
    INJECT_STATE_ACTIVE = 1U,
    INJECT_STATE_COMPLETE = 2U,
    INJECT_STATE_FAULT = 3U
} InjectState;

typedef enum {
    INJECT_RESULT_NONE = 0U,
    INJECT_RESULT_OK = 1U,
    INJECT_RESULT_ABORTED = 2U,
    INJECT_RESULT_TIMEOUT = 3U,
    INJECT_RESULT_FAULT = 4U,
    INJECT_RESULT_CURRENT_LIMIT = 5U
} InjectResult;

typedef struct {
    volatile uint32_t state;
    volatile uint32_t vector;
    volatile uint32_t duty_idx;
    volatile uint32_t duration_idx;
    volatile uint32_t active_ticks;
    volatile uint32_t max_ticks;
    volatile uint32_t result;
    volatile uint32_t end_faults;
    volatile uint32_t last_end_ms;
    volatile uint16_t start_raw14;
    volatile uint16_t end_raw14;
    volatile uint32_t abs_peak_adc_b;
    volatile uint32_t abs_peak_adc_c;
    volatile uint16_t adc_b_offset;
    volatile uint16_t adc_c_offset;
    volatile uint32_t sample_divider;
} InjectRuntime;

static InjectRuntime inject;
static uint32_t inject_last_response_ms;

static uint32_t inject_status_word(void)
{
    return inject.state |
           (inject.result << 4) |
           (inject.vector << 8) |
           (inject.duty_idx << 16) |
           (inject.duration_idx << 24);
}

void inject_force_safe(void)
{
    gpio_bit_reset(ENABLE_PIN);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_U, SVPWM_PERIOD);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_V, SVPWM_PERIOD);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_W, SVPWM_PERIOD);
}

static void inject_apply_pattern(void)
{
    const uint32_t duty = inject_duty_permille[inject.duty_idx];
    const uint8_t *high = inject_vector_high[inject.vector];
    const uint32_t compare = ((uint32_t)SVPWM_PERIOD * (1000U - duty)) / 1000U;
    const uint32_t low_compare = SVPWM_PERIOD;

    timer_channel_output_pulse_value_config(
        TIM_PWM, TIM_CH_U, high[0] != 0U ? compare : low_compare);
    timer_channel_output_pulse_value_config(
        TIM_PWM, TIM_CH_V, high[1] != 0U ? compare : low_compare);
    timer_channel_output_pulse_value_config(
        TIM_PWM, TIM_CH_W, high[2] != 0U ? compare : low_compare);
}

static void inject_measure_offsets(void)
{
    uint32_t sum_b = 0U;
    uint32_t sum_c = 0U;
    const uint32_t samples = 64U;

    for (uint32_t i = 0U; i < samples + 8U; i++) {
        analog_sample(&controller);
        if (i >= 8U) {
            sum_b += (uint32_t)controller.adc_b_raw;
            sum_c += (uint32_t)controller.adc_c_raw;
        }
    }
    inject.adc_b_offset = (uint16_t)(sum_b / samples);
    inject.adc_c_offset = (uint16_t)(sum_c / samples);
    controller.adc_b_offset = (int)inject.adc_b_offset;
    controller.adc_c_offset = (int)inject.adc_c_offset;
}

static void inject_finish(uint32_t result)
{
    gpio_bit_reset(ENABLE_PIN);
    inject_force_safe();
    inject.state = result == INJECT_RESULT_OK ? INJECT_STATE_COMPLETE
                                              : INJECT_STATE_FAULT;
    inject.result = result;
    inject.end_faults = safety_get_faults();
    inject.end_raw14 = comm_encoder.raw14;
    inject.last_end_ms = systick_uptime_ms();
}

static void inject_sample_current_watchdog(void)
{
    int32_t dev_b;
    int32_t dev_c;

    analog_sample(&controller);
    dev_b = (int32_t)controller.adc_b_raw - (int32_t)inject.adc_b_offset;
    dev_c = (int32_t)controller.adc_c_raw - (int32_t)inject.adc_c_offset;
    if (dev_b < 0) dev_b = -dev_b;
    if (dev_c < 0) dev_c = -dev_c;
    if ((uint32_t)dev_b > inject.abs_peak_adc_b) inject.abs_peak_adc_b = (uint32_t)dev_b;
    if ((uint32_t)dev_c > inject.abs_peak_adc_c) inject.abs_peak_adc_c = (uint32_t)dev_c;
    if ((uint32_t)dev_b > INJECT_ADC_DEVIATION_LIMIT ||
        (uint32_t)dev_c > INJECT_ADC_DEVIATION_LIMIT) {
        inject_finish(INJECT_RESULT_CURRENT_LIMIT);
    }
}

void inject_timer_tick(void)
{
    if (inject.state == INJECT_STATE_ACTIVE) {
        const uint32_t ticks = inject.active_ticks + 1U;
        inject.active_ticks = ticks;

        if (ticks == 1U) {
            /* Re-check before the first real enable: a fault can arrive between
             * the CAN arm and the first timer tick. */
            if (safety_get_faults() != 0U ||
                gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
                inject_finish(INJECT_RESULT_FAULT);
                return;
            }
            /* Apply the pattern first; the shadow register loads it at the next
             * update event, so PA11 only sees the previous all-low state. */
            inject_apply_pattern();
            gpio_bit_set(ENABLE_PIN);
        } else {
            if ((++inject.sample_divider % 3U) == 0U) {
                inject_sample_current_watchdog();
                if (inject.state != INJECT_STATE_ACTIVE) {
                    return;
                }
            }
            if (ticks >= inject.max_ticks) {
                inject_finish(INJECT_RESULT_OK);
                return;
            }
            if (safety_get_faults() != 0U ||
                gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET ||
                gpio_output_bit_get(GPIOA, GPIO_PIN_11) == RESET) {
                inject_finish(INJECT_RESULT_FAULT);
            }
        }
        return;
    }

    /* Not active: the hardware must stay off on every timer tick. */
    inject_force_safe();
}

static void inject_send_response(uint8_t opcode, uint8_t sequence,
                                 uint8_t page, uint8_t status,
                                 uint32_t payload)
{
    DiagRequest request;
    can_trasnmit_message_struct response;

    request.opcode = opcode;
    request.sequence = sequence;
    request.page = page;
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

static void inject_request_fire(const InjectRequest *request)
{
    const uint32_t now = systick_uptime_ms();
    uint8_t status = DIAG_STATUS_OK;
    uint32_t payload = 0U;

    if (inject.state == INJECT_STATE_ACTIVE) {
        status = DIAG_STATUS_BUSY;
        payload = inject_status_word();
    } else if ((uint32_t)(now - inject.last_end_ms) < INJECT_COOLDOWN_MS) {
        status = DIAG_STATUS_BUSY;
        payload = inject_status_word();
    } else if (safety_get_faults() != 0U ||
               gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET ||
               gpio_output_bit_get(GPIOA, GPIO_PIN_11) != RESET) {
        status = DIAG_STATUS_UNAVAILABLE;
        payload = safety_get_faults();
    } else if (comm_encoder.valid == 0U || controller.adc_valid == 0U) {
        status = DIAG_STATUS_UNAVAILABLE;
        payload = (comm_encoder.valid != 0U ? 1U : 0U) |
                  (controller.adc_valid != 0U ? 2U : 0U);
    } else {
        inject.vector = request->vector;
        inject.duty_idx = request->duty_idx;
        inject.duration_idx = request->duration_idx;
        inject.max_ticks =
            (uint32_t)inject_duration_ms[request->duration_idx] *
            INJECT_TICKS_PER_MS;
        inject.active_ticks = 0U;
        inject.result = INJECT_RESULT_NONE;
        inject.end_faults = 0U;
        inject.abs_peak_adc_b = 0U;
        inject.abs_peak_adc_c = 0U;
        inject.sample_divider = 0U;
        inject.start_raw14 = comm_encoder.raw14;
        inject.state = INJECT_STATE_ACTIVE;
        payload = inject_status_word();
    }

    inject_send_response(DIAG_OPCODE_INJECT, request->sequence,
                         request->vector, status, payload);
}

static void inject_request_stop(uint8_t sequence)
{
    if (inject.state == INJECT_STATE_ACTIVE) {
        inject_finish(INJECT_RESULT_ABORTED);
    } else {
        inject_force_safe();
    }
    inject_send_response(DIAG_OPCODE_INJECT_STOP, sequence, 0U,
                         DIAG_STATUS_OK, inject_status_word());
}

void inject_handle_can(const can_receive_message_struct *message)
{
    if (message->rx_sfid != DIAG_CAN_REQUEST_ID ||
        message->rx_ff != CAN_FF_STANDARD ||
        message->rx_ft != CAN_FT_DATA || message->rx_dlen != 8U) {
        diagnostic_counters.rx_bad_format++;
        return;
    }

    if (message->rx_data[3] == DIAG_OPCODE_INJECT ||
        message->rx_data[3] == DIAG_OPCODE_INJECT_STOP) {
        InjectRequest request;
        uint32_t now;

        if (inject_protocol_parse(message->rx_data, &request) != 0) {
            diagnostic_counters.rx_bad_protocol++;
            return;
        }
        now = systick_uptime_ms();
        if ((uint32_t)(now - inject_last_response_ms) < 20U) {
            diagnostic_counters.rx_rate_drop++;
            return;
        }
        inject_last_response_ms = now;
        diagnostic_counters.rx_valid++;
        if (request.opcode == DIAG_OPCODE_INJECT_STOP) {
            inject_request_stop(request.sequence);
        } else {
            inject_request_fire(&request);
        }
        return;
    }

    /* Any other frame received while a pulse is active aborts it; the timer
     * tick confirms the PA11 drop on its next pass. */
    if (inject.state == INJECT_STATE_ACTIVE) {
        inject_finish(INJECT_RESULT_ABORTED);
    } else {
        inject_force_safe();
    }
    diagnostics_handle_can(message);
}

uint32_t inject_snapshot(uint8_t page, uint8_t *status)
{
    *status = DIAG_STATUS_OK;

    switch (page) {
    case 20U: return inject_status_word();
    case 21U:
        return (inject.abs_peak_adc_b & 0xFFFFU) |
               ((inject.abs_peak_adc_c & 0xFFFFU) << 16);
    case 22U:
        return (uint32_t)inject.start_raw14 |
               ((uint32_t)inject.end_raw14 << 16);
    case 23U:
        return inject.active_ticks | (inject.end_faults << 16);
    case 24U:
        drv.fsr1 = drv_read_FSR1(drv);
        drv.fsr2 = drv_read_FSR2(drv);
        return (uint32_t)drv.fsr1 | ((uint32_t)drv.fsr2 << 16);
    case 25U:
        return (uint32_t)drv_read_register(drv, DCR) |
               ((uint32_t)drv_read_register(drv, CSACR) << 16);
    case 26U:
        return (uint32_t)drv_read_register(drv, OCPCR);
    default:
        *status = DIAG_STATUS_BAD_PAGE;
        return 0U;
    }
}

void inject_uart_report(void)
{
    printf("I1 st=%lu res=%lu vec=%lu duty=%lu dur=%lu ticks=%lu peak_b=%lu peak_c=%lu enc=%u->%u flt=%08lx\r\n",
           inject.state, inject.result, inject.vector, inject.duty_idx,
           inject.duration_idx, inject.active_ticks, inject.abs_peak_adc_b,
           inject.abs_peak_adc_c, inject.start_raw14, inject.end_raw14,
           safety_get_faults());
}

void inject_init(void)
{
    inject.state = INJECT_STATE_IDLE;
    inject.result = INJECT_RESULT_NONE;
    inject.end_faults = 0U;
    inject.last_end_ms = 0U;
    inject.sample_divider = 0U;
    inject_force_safe();

    /* Configure the gate driver with PA11 low. COAST stays clear so PA11 is
     * the only power gate; sense and VDS over-current are enabled and latch. */
    drv_write_DCR(drv, DIS_CPUV_EN, DIS_GDF_EN, OTW_REP_EN, PWM_MODE_3X,
                  0, 0, 0, 0, CLR_FLT_RST);
    drv_write_CSACR(drv, CSA_FET_SP, VREF_DIV_2, 0, CSA_GAIN_40,
                    DIS_SEN_EN, 1, 1, 1, SEN_LVL_0_25);
    drv_write_OCPCR(drv, TRETRY_50US, DEADTIME_50NS, OCP_LATCH,
                    OCP_DEG_4US, VDS_LVL_0_45);
    drv.fsr1 = drv_read_FSR1(drv);
    drv.fsr2 = drv_read_FSR2(drv);

    inject_measure_offsets();
    inject_force_safe();
}

#endif /* BRINGUP_INJECT */
