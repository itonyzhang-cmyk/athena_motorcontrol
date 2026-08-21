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
/* The original DRV initialization path waits 10 ms after EN_GATE rises before
 * the first SPI command. Keep the same hardware-ready interval here; PWM
 * primary output remains disabled and all three compare values stay all-low
 * throughout this diagnostic-only window. */
#define DRV_WAKE_SETTLE_MS 10U
/* Values below are the register payloads, not the 16-bit SPI commands. Keep
 * them beside the matching drv_write_* arguments so a readiness check cannot
 * silently drift from the configuration it is meant to verify. */
#define DRV_DCR_CONFIG_VALUE \
    ((uint16_t)((OTW_REP_EN << 7) | (PWM_MODE_3X << 5) | CLR_FLT_RST))
#define DRV_DCR_VERIFY_MASK ((uint16_t)(0x07FFU & ~CLR_FLT_RST))
#define DRV_CSACR_CONFIG_VALUE \
    ((uint16_t)((VREF_DIV_2 << 9) | (CSA_GAIN_40 << 6) | \
                (1U << 4) | (1U << 3) | (1U << 2)))
#define DRV_OCPCR_CONFIG_VALUE \
    ((uint16_t)((TRETRY_50US << 10) | (OCP_DEG_4US << 4) | \
                VDS_LVL_0_45))
#define DRV_WAKE_TRANSFER_COUNT 8U

enum {
    DRV_READY_CLEAR_BOOT = 1U,
    DRV_READY_CLEAR_WAKE = 2U,
    DRV_READY_CLEAR_STOP = 3U,
    DRV_READY_CLEAR_UNKNOWN_FRAME = 4U
};

typedef enum {
    INJECT_STATE_IDLE = 0U,
    INJECT_STATE_PRECHARGE = 1U,
    INJECT_STATE_POEN_TEST = 2U,
    INJECT_STATE_ACTIVE = 3U,
    INJECT_STATE_COMPLETE = 4U,
    INJECT_STATE_FAULT = 5U
} InjectState;

typedef enum {
    INJECT_RESULT_NONE = 0U,
    INJECT_RESULT_OK = 1U,
    INJECT_RESULT_ABORTED = 2U,
    INJECT_RESULT_TIMEOUT = 3U,
    INJECT_RESULT_FAULT = 4U,
    INJECT_RESULT_CURRENT_LIMIT = 5U
} InjectResult;

typedef enum {
    DRV_WAKE_IDLE = 0U,
    DRV_WAKE_WAIT_SETTLE = 1U
} DrvWakeState;

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
    volatile uint32_t precharge_started_ms;
    volatile uint32_t poen_test_started_ms;
    volatile uint16_t start_raw14;
    volatile uint16_t end_raw14;
    volatile int32_t peak_adc_b;
    volatile int32_t peak_adc_c;
    volatile uint32_t abs_peak_adc_b;
    volatile uint32_t abs_peak_adc_c;
    volatile uint16_t adc_b_offset;
    volatile uint16_t adc_c_offset;
    volatile uint32_t sample_divider;
    volatile uint32_t drv_ready;
    volatile uint32_t drv_ready_clear_reason;
    volatile uint16_t drv_fsr1;
    volatile uint16_t drv_fsr2;
    volatile uint16_t drv_pre_fsr1;
    volatile uint16_t drv_pre_fsr2;
    volatile uint16_t drv_fault_fsr1;
    volatile uint16_t drv_fault_fsr2;
    volatile uint16_t drv_poen_fsr1;
    volatile uint16_t drv_poen_fsr2;
    volatile uint8_t drv_fault_capture_pending;
    volatile uint16_t drv_dcr;
    volatile uint16_t drv_csacr;
    volatile uint16_t drv_ocpcr;
    volatile uint16_t drv_wake_tx[DRV_WAKE_TRANSFER_COUNT];
    volatile uint16_t drv_wake_rx[DRV_WAKE_TRANSFER_COUNT];
    volatile uint8_t drv_wake_spi_status[DRV_WAKE_TRANSFER_COUNT];
    volatile uint32_t drv_wake_spi_ctl0;
    volatile uint32_t drv_wake_spi_ctl1;
    volatile uint32_t drv_wake_spi_stat;
    volatile uint32_t drv_wake_gpiob_ctl1;
    volatile uint32_t drv_wake_gpiob_octl;
    volatile uint32_t drv_wake_gpiob_istat;
    volatile uint32_t drv_wake_gpioa_ctl1;
    volatile uint32_t drv_wake_gpioa_octl;
    volatile uint32_t drv_wake_gpioa_istat;
    volatile uint32_t drv_wake_state;
    volatile uint32_t drv_wake_started_ms;
    volatile uint8_t drv_wake_sequence;
} InjectRuntime;

static InjectRuntime inject;
static uint32_t inject_last_response_ms;

static uint8_t inject_spi_status_code(int status)
{
    switch (status) {
    case SPI_TRANSFER_OK: return 0U;
    case SPI_TRANSFER_TBE_TIMEOUT: return 1U;
    case SPI_TRANSFER_RBNE_TIMEOUT: return 2U;
    default: return 3U;
    }
}

static uint16_t inject_drv_transfer(uint8_t index, uint16_t tx_word)
{
    uint16_t rx_word = 0xFFFFU;
    int status = drv_spi_transfer(&drv, tx_word, &rx_word);

    inject.drv_wake_tx[index] = tx_word;
    inject.drv_wake_rx[index] = rx_word;
    inject.drv_wake_spi_status[index] = inject_spi_status_code(status);
    return rx_word;
}

static uint8_t inject_all_drv_transfers_ok(void)
{
    for (uint8_t i = 0U; i < DRV_WAKE_TRANSFER_COUNT; ++i) {
        if (inject.drv_wake_spi_status[i] != 0U) {
            return 0U;
        }
    }
    return 1U;
}

static void inject_capture_drv_bus_state(void)
{
    inject.drv_wake_spi_ctl0 = SPI_CTL0(SPI1);
    inject.drv_wake_spi_ctl1 = SPI_CTL1(SPI1);
    inject.drv_wake_spi_stat = SPI_STAT(SPI1);
    inject.drv_wake_gpiob_ctl1 = GPIO_CTL1(GPIOB);
    inject.drv_wake_gpiob_octl = GPIO_OCTL(GPIOB);
    inject.drv_wake_gpiob_istat = GPIO_ISTAT(GPIOB);
    inject.drv_wake_gpioa_ctl1 = GPIO_CTL1(GPIOA);
    inject.drv_wake_gpioa_octl = GPIO_OCTL(GPIOA);
    inject.drv_wake_gpioa_istat = GPIO_ISTAT(GPIOA);
}

static uint32_t inject_status_word(void)
{
    return inject.state |
           (inject.result << 4) |
           (inject.vector << 8) |
           (inject.duty_idx << 16) |
           (inject.duration_idx << 24);
}

static void inject_clear_drv_ready(uint32_t reason)
{
    inject.drv_ready = 0U;
    inject.drv_ready_clear_reason = reason;
}

int inject_drv_wake_window_active(void)
{
    return inject.drv_wake_state == DRV_WAKE_WAIT_SETTLE;
}

void inject_force_safe(void)
{
    gpio_bit_reset(ENABLE_PIN);
    timer_primary_output_config(TIM_PWM, DISABLE);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_U, SVPWM_PERIOD);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_V, SVPWM_PERIOD);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_W, SVPWM_PERIOD);
}

static void inject_disable_pwm_outputs(void)
{
    inject_force_safe();
    timer_primary_output_config(TIM_PWM, DISABLE);
}

static void inject_hold_pwm_outputs_off(void)
{
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_U, SVPWM_PERIOD);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_V, SVPWM_PERIOD);
    timer_channel_output_pulse_value_config(TIM_PWM, TIM_CH_W, SVPWM_PERIOD);
    timer_primary_output_config(TIM_PWM, DISABLE);
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

static void inject_capture_fault_registers(void)
{
    if (inject.drv_fault_capture_pending == 0U) {
        return;
    }
    inject.drv_fault_capture_pending = 0U;
    /* PWM remains disabled; this bounded read is only for post-abort evidence. */
    gpio_bit_set(ENABLE_PIN);
    inject.drv_fault_fsr1 = drv_read_FSR1(drv);
    inject.drv_fault_fsr2 = drv_read_FSR2(drv);
    gpio_bit_reset(ENABLE_PIN);
}

static void inject_sample_current_watchdog(void)
{
    int32_t dev_b;
    int32_t dev_c;
    int32_t abs_b;
    int32_t abs_c;

    analog_sample(&controller);
    dev_b = (int32_t)controller.adc_b_raw - (int32_t)inject.adc_b_offset;
    dev_c = (int32_t)controller.adc_c_raw - (int32_t)inject.adc_c_offset;
    abs_b = dev_b < 0 ? -dev_b : dev_b;
    abs_c = dev_c < 0 ? -dev_c : dev_c;
    if ((uint32_t)abs_b > inject.abs_peak_adc_b) {
        inject.abs_peak_adc_b = (uint32_t)abs_b;
        inject.peak_adc_b = dev_b;
    }
    if ((uint32_t)abs_c > inject.abs_peak_adc_c) {
        inject.abs_peak_adc_c = (uint32_t)abs_c;
        inject.peak_adc_c = dev_c;
    }
    if ((uint32_t)abs_b > INJECT_ADC_DEVIATION_LIMIT ||
        (uint32_t)abs_c > INJECT_ADC_DEVIATION_LIMIT) {
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
                if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
                    inject.drv_fault_capture_pending = 1U;
                }
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
                if (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
                    inject.drv_fault_capture_pending = 1U;
                }
                inject_finish(INJECT_RESULT_FAULT);
            }
        }
        return;
    }

    if (inject.drv_wake_state == DRV_WAKE_WAIT_SETTLE) {
        /* The main-loop wake service owns PA11 for this bounded interval. */
        inject_hold_pwm_outputs_off();
        return;
    }

    if (inject.state == INJECT_STATE_PRECHARGE ||
        inject.state == INJECT_STATE_POEN_TEST) {
        /* EN_GATE is high, but PWM/POEN and all switching inputs stay off. */
        inject_hold_pwm_outputs_off();
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

    if (inject.state == INJECT_STATE_PRECHARGE ||
        inject.state == INJECT_STATE_ACTIVE) {
        status = DIAG_STATUS_BUSY;
        payload = inject_status_word();
    } else if ((uint32_t)(now - inject.last_end_ms) < INJECT_COOLDOWN_MS) {
        status = DIAG_STATUS_BUSY;
        payload = inject_status_word();
    } else if (safety_get_faults() != 0U ||
               inject.drv_ready == 0U ||
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
        inject.peak_adc_b = 0;
        inject.peak_adc_c = 0;
        inject.sample_divider = 0U;
        inject.start_raw14 = comm_encoder.raw14;
        /* Let the DRV8323 charge pump settle before any PWM edge. */
        inject.precharge_started_ms = now;
        inject.state = INJECT_STATE_PRECHARGE;
        inject_disable_pwm_outputs();
        gpio_bit_set(ENABLE_PIN);
        payload = inject_status_word();
    }

    inject_send_response(DIAG_OPCODE_INJECT, request->sequence,
                         request->vector, status, payload);
}

static void inject_start_drv_wake(uint8_t sequence)
{
    inject_clear_drv_ready(DRV_READY_CLEAR_WAKE);
    if (inject.state == INJECT_STATE_PRECHARGE ||
        inject.state == INJECT_STATE_ACTIVE ||
        inject.drv_wake_state != DRV_WAKE_IDLE) {
        inject_send_response(DIAG_OPCODE_DRV_WAKE, sequence, 0U,
                             DIAG_STATUS_BUSY, 0U);
        return;
    }
    if ((safety_get_faults() & ~SAFETY_FAULT_GATE_DRIVER) != 0U ||
        (gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET &&
         (safety_get_faults() & SAFETY_FAULT_GATE_DRIVER) == 0U)) {
        inject_send_response(DIAG_OPCODE_DRV_WAKE, sequence, 0U,
                             DIAG_STATUS_UNAVAILABLE, 0U);
        return;
    }

    /* The ISR only begins a timer-bounded wake window. SPI access and the
     * response occur in inject_service() so SysTick/CAN cannot deadlock. */
    inject.drv_wake_sequence = sequence;
    inject.drv_wake_started_ms = systick_uptime_ms();
    inject.drv_wake_state = DRV_WAKE_WAIT_SETTLE;
    for (uint8_t i = 0U; i < DRV_WAKE_TRANSFER_COUNT; ++i) {
        inject.drv_wake_tx[i] = 0U;
        inject.drv_wake_rx[i] = 0U;
        inject.drv_wake_spi_status[i] = 3U;
    }
    inject_disable_pwm_outputs();
    gpio_bit_set(ENABLE_PIN);
}

void inject_service(void)
{
    uint8_t status = DIAG_STATUS_UNAVAILABLE;
    const int nFAULT_ok = gpio_input_bit_get(GPIOA, GPIO_PIN_12) != RESET;

    if (inject.drv_wake_state == DRV_WAKE_IDLE &&
        inject.state != INJECT_STATE_ACTIVE) {
        inject_capture_fault_registers();
    }

    if (inject.state == INJECT_STATE_PRECHARGE) {
        if ((uint32_t)(systick_uptime_ms() - inject.precharge_started_ms) <
            DRV_WAKE_SETTLE_MS) {
            return;
        }
        if (safety_get_faults() != 0U ||
            gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET ||
            gpio_output_bit_get(GPIOA, GPIO_PIN_11) == RESET) {
            inject_finish(INJECT_RESULT_FAULT);
            return;
        }
        /* Isolate POEN with all three compare registers still all-low. */
        timer_primary_output_config(TIM_PWM, ENABLE);
        inject.poen_test_started_ms = systick_uptime_ms();
        inject.state = INJECT_STATE_POEN_TEST;
        return;
    }

    if (inject.state == INJECT_STATE_POEN_TEST) {
        if ((uint32_t)(systick_uptime_ms() - inject.poen_test_started_ms) <
            DRV_WAKE_SETTLE_MS) {
            return;
        }
        gpio_bit_set(ENABLE_PIN);
        inject.drv_poen_fsr1 = drv_read_FSR1(drv);
        inject.drv_poen_fsr2 = drv_read_FSR2(drv);
        gpio_bit_reset(ENABLE_PIN);
        if (inject.drv_poen_fsr1 != 0U || inject.drv_poen_fsr2 != 0U ||
            safety_get_faults() != 0U ||
            gpio_input_bit_get(GPIOA, GPIO_PIN_12) == RESET) {
            inject_finish(INJECT_RESULT_FAULT);
            return;
        }
        inject.state = INJECT_STATE_ACTIVE;
        return;
    }

    if (inject.drv_wake_state != DRV_WAKE_WAIT_SETTLE ||
        (uint32_t)(systick_uptime_ms() - inject.drv_wake_started_ms) <
            DRV_WAKE_SETTLE_MS) {
        return;
    }

    /* Read once even when nFAULT is already low. PWM/POEN remain disabled;
     * this is the only way to distinguish a real DRV fault from an absent SDO
     * response. The EXTI handler leaves PA11 high during this bounded window. */
    inject.drv_pre_fsr1 = drv_read_FSR1(drv);
    inject.drv_pre_fsr2 = drv_read_FSR2(drv);
    inject_drv_transfer(0U, (uint16_t)((DCR << 11) | DRV_DCR_CONFIG_VALUE));
    inject_drv_transfer(1U, (uint16_t)((CSACR << 11) | DRV_CSACR_CONFIG_VALUE));
    inject_drv_transfer(2U, (uint16_t)((OCPCR << 11) | DRV_OCPCR_CONFIG_VALUE));
    inject.drv_fsr1 = inject_drv_transfer(3U, (uint16_t)(0x8000U | (FSR1 << 11)));
    inject.drv_fsr2 = inject_drv_transfer(4U, (uint16_t)(0x8000U | (FSR2 << 11)));
    inject.drv_dcr = inject_drv_transfer(5U, (uint16_t)(0x8000U | (DCR << 11)));
    inject.drv_csacr = inject_drv_transfer(6U, (uint16_t)(0x8000U | (CSACR << 11)));
    inject.drv_ocpcr = inject_drv_transfer(7U, (uint16_t)(0x8000U | (OCPCR << 11)));
    inject_capture_drv_bus_state();
    /* CLR_FLT is self-clearing, so its readback is intentionally not part of
     * the DCR comparison. A low nFAULT still makes the wake unavailable. */
    if (nFAULT_ok && inject_all_drv_transfers_ok() != 0U &&
        inject.drv_fsr1 == 0U && inject.drv_fsr2 == 0U &&
        (inject.drv_dcr & DRV_DCR_VERIFY_MASK) ==
            (DRV_DCR_CONFIG_VALUE & DRV_DCR_VERIFY_MASK) &&
        inject.drv_csacr == DRV_CSACR_CONFIG_VALUE &&
        inject.drv_ocpcr == DRV_OCPCR_CONFIG_VALUE) {
        inject.drv_ready = 1U;
        status = DIAG_STATUS_OK;
    }
    inject_force_safe();
    inject.drv_wake_state = DRV_WAKE_IDLE;
    inject_send_response(DIAG_OPCODE_DRV_WAKE, inject.drv_wake_sequence, 0U,
                         status, inject.drv_ready);
}

static void inject_request_stop(uint8_t sequence)
{
    inject.drv_wake_state = DRV_WAKE_IDLE;
    inject_clear_drv_ready(DRV_READY_CLEAR_STOP);
    if (inject.state == INJECT_STATE_PRECHARGE ||
        inject.state == INJECT_STATE_ACTIVE) {
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

    if (message->rx_data[3] == DIAG_OPCODE_DRV_WAKE) {
        DiagRequest request;
        if (diag_protocol_parse(message->rx_data, &request) != 0 ||
            request.page != 0U) {
            diagnostic_counters.rx_bad_protocol++;
            return;
        }
        inject_start_drv_wake(request.sequence);
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

    /* Read-only diagnostics must not clear a successful DRV wake result; the
     * host queries pages 27..48 immediately after opcode 0x06. Only a control
     * or unknown frame invalidates the readiness latch. */
    if (message->rx_data[3] <= DIAG_OPCODE_GET_COUNTER) {
        diagnostics_handle_can(message);
        return;
    }

    /* Any other frame received while a pulse is active aborts it; the timer
     * tick confirms the PA11 drop on its next pass. */
    if (inject.state == INJECT_STATE_PRECHARGE ||
        inject.state == INJECT_STATE_ACTIVE) {
        inject_finish(INJECT_RESULT_ABORTED);
    } else {
        inject.drv_wake_state = DRV_WAKE_IDLE;
        inject_clear_drv_ready(DRV_READY_CLEAR_UNKNOWN_FRAME);
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
        return ((uint32_t)(uint16_t)inject.peak_adc_b) |
               (((uint32_t)(uint16_t)inject.peak_adc_c) << 16);
    case 22U:
        return (uint32_t)inject.start_raw14 |
               ((uint32_t)inject.end_raw14 << 16);
    case 23U:
        return inject.active_ticks | (inject.end_faults << 16);
    case 27U: return inject.drv_ready;
    case 28U:
        return (uint32_t)inject.drv_dcr | ((uint32_t)inject.drv_csacr << 16);
    case 29U:
        return (uint32_t)inject.drv_fsr1 | ((uint32_t)inject.drv_fsr2 << 16);
    case 30U: return inject.drv_ocpcr;
    case 31U:
        return (uint32_t)inject.drv_wake_spi_status[0] |
               ((uint32_t)inject.drv_wake_spi_status[1] << 2) |
               ((uint32_t)inject.drv_wake_spi_status[2] << 4) |
               ((uint32_t)inject.drv_wake_spi_status[3] << 6) |
               ((uint32_t)inject.drv_wake_spi_status[4] << 8) |
               ((uint32_t)inject.drv_wake_spi_status[5] << 10) |
               ((uint32_t)inject.drv_wake_spi_status[6] << 12) |
               ((uint32_t)inject.drv_wake_spi_status[7] << 14);
    case 32U: return (uint32_t)inject.drv_wake_tx[0] | ((uint32_t)inject.drv_wake_rx[0] << 16);
    case 33U: return (uint32_t)inject.drv_wake_tx[1] | ((uint32_t)inject.drv_wake_rx[1] << 16);
    case 34U: return (uint32_t)inject.drv_wake_tx[2] | ((uint32_t)inject.drv_wake_rx[2] << 16);
    case 35U: return (uint32_t)inject.drv_wake_tx[3] | ((uint32_t)inject.drv_wake_rx[3] << 16);
    case 36U: return (uint32_t)inject.drv_wake_tx[4] | ((uint32_t)inject.drv_wake_rx[4] << 16);
    case 37U: return (uint32_t)inject.drv_wake_tx[5] | ((uint32_t)inject.drv_wake_rx[5] << 16);
    case 38U: return (uint32_t)inject.drv_wake_tx[6] | ((uint32_t)inject.drv_wake_rx[6] << 16);
    case 39U: return (uint32_t)inject.drv_wake_tx[7] | ((uint32_t)inject.drv_wake_rx[7] << 16);
    case 40U: return inject.drv_wake_spi_ctl0;
    case 41U: return inject.drv_wake_spi_ctl1;
    case 42U: return inject.drv_wake_spi_stat;
    case 43U: return inject.drv_wake_gpiob_ctl1;
    case 44U: return inject.drv_wake_gpiob_octl;
    case 45U: return inject.drv_wake_gpiob_istat;
    case 46U: return inject.drv_wake_gpioa_ctl1;
    case 47U: return inject.drv_wake_gpioa_octl;
    case 48U: return inject.drv_wake_gpioa_istat;
    case 49U: return inject.drv_ready_clear_reason;
    case 50U: return inject.drv_pre_fsr1;
    case 51U: return inject.drv_pre_fsr2;
    case 52U: return inject.drv_fault_fsr1;
    case 53U: return inject.drv_fault_fsr2;
    case 54U: return inject.drv_poen_fsr1;
    case 55U: return inject.drv_poen_fsr2;
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
    printf("I1 st=%lu res=%lu vec=%lu duty=%lu dur=%lu ticks=%lu peak_b=%ld peak_c=%ld enc=%u->%u flt=%08lx\r\n",
           inject.state, inject.result, inject.vector, inject.duty_idx,
           inject.duration_idx, inject.active_ticks, inject.peak_adc_b,
           inject.peak_adc_c, inject.start_raw14, inject.end_raw14,
           safety_get_faults());
}

void inject_init(void)
{
    inject.state = INJECT_STATE_IDLE;
    inject.result = INJECT_RESULT_NONE;
    inject.end_faults = 0U;
    inject.last_end_ms = 0U;
    inject.sample_divider = 0U;
    inject_clear_drv_ready(DRV_READY_CLEAR_BOOT);
    inject.drv_wake_state = DRV_WAKE_IDLE;
    inject_force_safe();

    /* DRV SPI writes are deferred until the explicit drv-wake command. */
    inject_measure_offsets();
    inject_force_safe();
}

#endif /* BRINGUP_INJECT */
