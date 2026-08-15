#include "diagnostics.h"

#ifndef STM32F446

#include <stdio.h>
#include "adc.h"
#include "can.h"
#include "diag_protocol.h"
#include "gpio.h"
#include "hw_config.h"
#include "safety.h"
#include "structs.h"
#include "systick.h"
#include "tim.h"
#include "usart.h"
#ifdef BRINGUP_INJECT
#include "inject.h"
#endif

DiagnosticCounters diagnostic_counters;

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
#ifdef BRINGUP_INJECT
        case 20U: /* fallthrough to shared handler */
        case 21U:
        case 22U:
        case 23U:
        case 24U:
        case 25U:
        case 26U:
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

void diagnostics_handle_can(const can_receive_message_struct *message)
{
    static uint32_t last_response_ms;
    DiagRequest request;
    can_trasnmit_message_struct response;
    uint8_t status;
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

    payload = diagnostic_payload(&request, &status);
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
