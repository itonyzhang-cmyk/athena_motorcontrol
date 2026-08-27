#include <errno.h>
#include <getopt.h>
#include <libusb.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define UC12_VID 0x0471
#define UC12_PID 0x1200
#define UC12_INTERFACE 0
#define UC12_EP_COMMAND_OUT 0x01
#define UC12_EP_COMMAND_IN 0x81
#define UC12_EP_CAN_TX 0x02
#define UC12_EP_CAN_RX 0x82
#define UC12_USB_TIMEOUT_MS 1000
#define UC12_RECORD_SIZE 19

#define DIAG_REQUEST_ID 0x701U
#define DIAG_RESPONSE_ID 0x781U
#define DIAG_PROTOCOL_MAJOR 1U

#define OPCODE_PING 0x00U
#define OPCODE_INFO 0x01U
#define OPCODE_SNAPSHOT 0x02U
#define OPCODE_COUNTER 0x03U
#define OPCODE_INJECT 0x04U
#define OPCODE_STOP 0x05U
#define OPCODE_DRV_WAKE 0x06U
#define OPCODE_CONTROL 0x07U

/* Must match the BRINGUP_INJECT firmware tables exactly. */
static const double inject_duty_percent[] = {
    0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0
};
static const double inject_duration_ms[] = {10.0, 20.0, 30.0, 40.0, 50.0};
#define INJECT_VECTOR_COUNT 6U
#define INJECT_RESULT_OK 1U
#define INJECT_RESULT_ABORTED 2U
#define INJECT_RESULT_TIMEOUT 3U
#define INJECT_RESULT_FAULT 4U
#define INJECT_RESULT_CURRENT_LIMIT 5U

static volatile sig_atomic_t keep_running = 1;

struct options {
    unsigned channel;
    unsigned timeout_ms;
    unsigned interval_ms;
    unsigned seconds;
    int confirm_inject;
    int confirm_drv_wake;
    const char *csv_path;
    double supply_volts;
    double current_limit_amps;
    int have_supply_volts;
    int have_current_limit;
};

struct response {
    uint8_t opcode;
    uint8_t sequence;
    uint8_t page;
    uint8_t status;
    uint32_t payload;
};

struct rx_stream {
    uint8_t bytes[4096];
    size_t length;
};

struct client {
    libusb_context *context;
    libusb_device_handle *usb;
    struct rx_stream rx;
    struct options options;
    uint8_t sequence;
};

struct query_page {
    uint8_t opcode;
    uint8_t page;
};

static const uint8_t snapshot_pages[] = {
    0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    116, 117, 118, 119, 120, 121, 122
};

static const uint8_t counter_pages[] = {9, 10, 11, 12, 13};

static const uint8_t inject_result_pages[] = {20, 21, 22, 23};
static const uint8_t drv_pages[] = {24, 25, 26};
static const uint8_t drv_wake_pages[] = {
    27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55
};
static const uint8_t normal_drv_status_pages[] = {2, 3, 24, 25, 26, 27, 28, 29, 30, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122};

static void print_response(const struct response *response);

static void on_signal(int signal_number)
{
    (void)signal_number;
    keep_running = 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static uint64_t realtime_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000U + (uint64_t)ts.tv_nsec / 1000U;
}

static void sleep_ms(unsigned milliseconds)
{
    struct timespec request;
    request.tv_sec = (time_t)(milliseconds / 1000U);
    request.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&request, &request) != 0 && errno == EINTR && keep_running) {
    }
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint8_t crc8_atm(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    size_t i;
    unsigned bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) ? (uint8_t)((crc << 1) ^ 0x07U)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static void build_request(uint8_t opcode, uint8_t sequence, uint8_t page,
                          uint8_t data[8])
{
    data[0] = 0xA5U;
    data[1] = 0x5AU;
    data[2] = DIAG_PROTOCOL_MAJOR;
    data[3] = opcode;
    data[4] = sequence;
    data[5] = page;
    data[6] = 0U;
    data[7] = crc8_atm(data, 7U);
}

static void build_control_request(uint8_t sequence, uint8_t page,
                                  uint8_t argument, uint8_t data[8])
{
    build_request(OPCODE_CONTROL, sequence, page, data);
    data[6] = argument;
    data[7] = crc8_atm(data, 7U);
}

static void build_inject_request(uint8_t opcode, uint8_t sequence,
                                 uint8_t vector, uint8_t duty_idx,
                                 uint8_t duration_idx, uint8_t data[8])
{
    data[0] = 0xA5U;
    data[1] = 0x5AU;
    data[2] = DIAG_PROTOCOL_MAJOR;
    data[3] = opcode;
    data[4] = sequence;
    data[5] = vector;
    data[6] = (uint8_t)((duration_idx << 4) | (duty_idx & 0x0FU));
    data[7] = crc8_atm(data, 7U);
}

static int parse_response_record(const uint8_t record[UC12_RECORD_SIZE],
                                 unsigned expected_channel,
                                 uint8_t opcode, uint8_t sequence, uint8_t page,
                                 struct response *response)
{
    uint8_t flags = record[6];
    unsigned channel = (flags >> 4) & 0x03U;
    unsigned remote = (flags >> 6) & 0x01U;
    unsigned extended = (flags >> 7) & 0x01U;
    unsigned dlc = flags & 0x0FU;
    uint32_t id = read_le32(record + 7) >> (extended ? 3 : 5);
    const uint8_t *data = record + 11;

    if (record[4] != 0U || channel != expected_channel || remote || extended ||
        dlc != 8U || id != DIAG_RESPONSE_ID) {
        return 0;
    }
    if (data[0] != (uint8_t)(opcode | 0x80U) || data[1] != sequence ||
        data[2] != page || data[3] > 4U) {
        return 0;
    }
    response->opcode = opcode;
    response->sequence = sequence;
    response->page = page;
    response->status = data[3];
    response->payload = read_le32(data + 4);
    return 1;
}

static int uc12_command(libusb_device_handle *handle,
                        const uint8_t *request, int request_length)
{
    uint8_t response[64];
    int transferred = 0;
    int rc;

    rc = libusb_bulk_transfer(handle, UC12_EP_COMMAND_OUT,
                              (unsigned char *)request, request_length,
                              &transferred, UC12_USB_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS || transferred != request_length) {
        fprintf(stderr, "UC12 command write failed: %s (%d/%d bytes)\n",
                libusb_error_name(rc), transferred, request_length);
        return -1;
    }
    transferred = 0;
    rc = libusb_bulk_transfer(handle, UC12_EP_COMMAND_IN, response,
                              (int)sizeof(response), &transferred,
                              UC12_USB_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "UC12 command response failed: %s\n",
                libusb_error_name(rc));
        return -1;
    }
    if (transferred < 3 || response[0] != 0x12U ||
        response[1] != request[1] || !(response[2] & 0x80U) ||
        transferred < (int)(3U + (response[2] & 0x7FU))) {
        fprintf(stderr, "Unexpected UC12 response to command 0x%02X\n",
                request[1]);
        return -1;
    }
    return 0;
}

static int start_can(struct client *client)
{
    const uint8_t init[] = {
        0x12, 0x03, 0x04, 0x00,
        (uint8_t)(client->options.channel << 4), 0x00, 0x14
    };
    const uint8_t filter_all[] = {
        0x12, 0x04, 0x0A, 0x00,
        (uint8_t)((client->options.channel << 4) | 0x40U),
        0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF
    };
    const uint8_t start[] = {
        0x12, 0x0E, 0x02, 0x00, (uint8_t)(client->options.channel << 4)
    };
    return uc12_command(client->usb, init, (int)sizeof(init)) ||
           uc12_command(client->usb, filter_all, (int)sizeof(filter_all)) ||
           uc12_command(client->usb, start, (int)sizeof(start)) ? -1 : 0;
}

static int open_client(struct client *client)
{
    int rc = libusb_init(&client->context);
    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "libusb init failed: %s\n", libusb_error_name(rc));
        return -1;
    }
    client->usb = libusb_open_device_with_vid_pid(client->context,
                                                   UC12_VID, UC12_PID);
    if (!client->usb) {
        fprintf(stderr, "UC12 0471:1200 not found. Stop SavvyCAN/WebUI and check USB.\n");
        return -1;
    }
    libusb_set_auto_detach_kernel_driver(client->usb, 1);
    rc = libusb_claim_interface(client->usb, UC12_INTERFACE);
    if (rc != LIBUSB_SUCCESS) {
        fprintf(stderr, "Cannot claim UC12 interface: %s. Another program may own it.\n",
                libusb_error_name(rc));
        return -1;
    }
    return start_can(client);
}

static void close_client(struct client *client)
{
    if (client->usb) {
        libusb_release_interface(client->usb, UC12_INTERFACE);
        libusb_close(client->usb);
    }
    if (client->context) libusb_exit(client->context);
}

static int transmit_request(struct client *client, const uint8_t data[8])
{
    uint8_t record[UC12_RECORD_SIZE] = {0};
    uint8_t confirmation[64];
    int transferred = 0;
    int rc;

    record[6] = (uint8_t)((client->options.channel << 4) | 8U);
    write_le32(record + 7, DIAG_REQUEST_ID << 5);
    memcpy(record + 11, data, 8);
    rc = libusb_bulk_transfer(client->usb, UC12_EP_CAN_TX, record,
                              UC12_RECORD_SIZE, &transferred,
                              UC12_USB_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS || transferred != UC12_RECORD_SIZE) {
        fprintf(stderr, "UC12 CAN request failed: %s (%d/%d bytes)\n",
                libusb_error_name(rc), transferred, UC12_RECORD_SIZE);
        return -1;
    }
    transferred = 0;
    rc = libusb_bulk_transfer(client->usb, UC12_EP_COMMAND_IN, confirmation,
                              (int)sizeof(confirmation), &transferred,
                              UC12_USB_TIMEOUT_MS);
    if (rc != LIBUSB_SUCCESS || transferred < 3 || confirmation[2] < 1U) {
        fprintf(stderr,
                "UC12 did not confirm the diagnostic request (rc=%s "
                "transferred=%d bytes=%02x %02x %02x %02x)\n",
                libusb_error_name(rc), transferred,
                transferred > 0 ? confirmation[0] : 0,
                transferred > 1 ? confirmation[1] : 0,
                transferred > 2 ? confirmation[2] : 0,
                transferred > 3 ? confirmation[3] : 0);
        return -1;
    }
    return 0;
}

static int read_record(struct client *client,
                       uint8_t record[UC12_RECORD_SIZE], unsigned timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    while (keep_running) {
        int transferred = 0;
        int rc;
        unsigned remaining;
        if (client->rx.length >= UC12_RECORD_SIZE) {
            memcpy(record, client->rx.bytes, UC12_RECORD_SIZE);
            client->rx.length -= UC12_RECORD_SIZE;
            memmove(client->rx.bytes, client->rx.bytes + UC12_RECORD_SIZE,
                    client->rx.length);
            return 1;
        }
        if (monotonic_ms() >= deadline) return 0;
        remaining = (unsigned)(deadline - monotonic_ms());
        if (remaining > 100U) remaining = 100U;
        rc = libusb_bulk_transfer(client->usb, UC12_EP_CAN_RX,
                                  client->rx.bytes + client->rx.length,
                                  (int)(sizeof(client->rx.bytes) - client->rx.length),
                                  &transferred, remaining ? remaining : 1U);
        if (rc == LIBUSB_ERROR_TIMEOUT || rc == LIBUSB_ERROR_INTERRUPTED) continue;
        if (rc != LIBUSB_SUCCESS) {
            fprintf(stderr, "UC12 receive failed: %s\n", libusb_error_name(rc));
            return -1;
        }
        client->rx.length += (size_t)transferred;
        if (client->rx.length == sizeof(client->rx.bytes) &&
            client->rx.length < UC12_RECORD_SIZE) {
            return -1;
        }
    }
    return 0;
}

static int query_raw(struct client *client, const uint8_t request[8],
                     uint8_t opcode, uint8_t page, struct response *response)
{
    uint8_t record[UC12_RECORD_SIZE];
    uint8_t sequence = request[4];
    unsigned attempt;

    for (attempt = 0; attempt < 2U && keep_running; ++attempt) {
        uint64_t deadline = monotonic_ms() + client->options.timeout_ms;

        if (transmit_request(client, request) != 0) return -1;
        while (keep_running && monotonic_ms() < deadline) {
            unsigned remaining = (unsigned)(deadline - monotonic_ms());
            int rc = read_record(client, record, remaining);
            if (rc < 0) return -1;
            if (rc == 0) break;
            if (parse_response_record(record, client->options.channel, opcode,
                                      sequence, page, response)) {
                return 0;
            }
        }
        if (attempt == 0U && opcode <= 3U) {
            fprintf(stderr,
                    "ATHENA-DIAG opcode=%u page=%u missed; retrying once.\n",
                    opcode, page);
            continue;
        }
        break;
    }
    fprintf(stderr, "Timeout waiting for ATHENA-DIAG opcode=%u page=%u.\n",
            opcode, page);
    return -1;
}

static int query(struct client *client, uint8_t opcode, uint8_t page,
                 struct response *response)
{
    uint8_t request[8];
    uint8_t sequence = ++client->sequence;
    build_request(opcode, sequence, page, request);
    return query_raw(client, request, opcode, page, response);
}

static int control(struct client *client, uint8_t page, uint8_t argument,
                   struct response *response)
{
    uint8_t request[8];
    uint8_t sequence = ++client->sequence;
    build_control_request(sequence, page, argument, request);
    return query_raw(client, request, OPCODE_CONTROL, page, response);
}

static const char *status_name(uint8_t status)
{
    static const char *names[] = {
        "OK", "BAD_PAGE", "BUSY", "UNAVAILABLE", "UNSUPPORTED"
    };
    return status < 5U ? names[status] : "INVALID";
}

static const char *snapshot_name(uint8_t page)
{
    switch (page) {
    case 0: return "sample_seq";
    case 1: return "uptime_ms";
    case 2: return "safety_fault_latched";
    case 3: return "safety_digital_flags";
    case 4: return "encoder_frame_raw14";
    case 5: return "encoder_count";
    case 6: return "encoder_turns";
    case 9: return "phase_adc_b_c";
    case 10: return "vbus_adc_raw";
    case 11: return "temperature_adc_raw";
    case 12: return "hall0_hall1";
    case 13: return "hall2_hall3";
    case 14: return "hall4_hall5";
    case 15: return "encoder_diaagc_mag";
    case 16: return "can_error";
    case 17: return "loop_count";
    case 18: return "timer_ch0_ch1";
    case 19: return "timer_ch2_period";
    case 20: return "inject_status";
    case 21: return "inject_peak_currents";
    case 22: return "inject_encoder_delta";
    case 23: return "inject_ticks_faults";
    case 24: return "drv_fsr1_fsr2";
    case 25: return "drv_dcr_csacr";
    case 26: return "drv_ocpcr";
    case 27: return "drv_ready";
    case 28: return "drv_wake_dcr_csacr";
    case 29: return "drv_wake_fsr1_fsr2";
    case 30: return "drv_wake_ocpcr";
    case 31: return "drv_wake_spi_status";
    case 32: return "drv_xfer0_dcr_tx_rx";
    case 33: return "drv_xfer1_csacr_tx_rx";
    case 34: return "drv_xfer2_ocpcr_tx_rx";
    case 35: return "drv_xfer3_fsr1_tx_rx";
    case 36: return "drv_xfer4_fsr2_tx_rx";
    case 37: return "drv_xfer5_dcr_tx_rx";
    case 38: return "drv_xfer6_csacr_tx_rx";
    case 39: return "drv_xfer7_ocpcr_tx_rx";
    case 40: return "drv_spi1_ctl0";
    case 41: return "drv_spi1_ctl1";
    case 42: return "drv_spi1_stat";
    case 43: return "drv_gpiob_ctl1";
    case 44: return "drv_gpiob_octl";
    case 45: return "drv_gpiob_istat";
    case 46: return "drv_gpioa_ctl1";
    case 47: return "drv_gpioa_octl";
    case 48: return "drv_gpioa_istat";
    case 49: return "drv_ready_clear_reason";
    case 50: return "drv_pre_fsr1";
    case 51: return "drv_pre_fsr2";
    case 52: return "drv_fault_fsr1";
    case 53: return "drv_fault_fsr2";
    case 54: return "drv_poen_fsr1";
    case 55: return "drv_poen_fsr2";
    case 56: return "drv_init_reason";
    case 57: return "drv_init_pre_fsr1_fsr2";
    case 58: return "drv_init_final_fsr1_fsr2";
    case 59: return "drv_init_dcr_csacr";
    case 60: return "drv_init_ocpcr";
    case 61: return "drv_init_spi_rx0";
    case 62: return "drv_init_spi_rx1";
    case 63: return "drv_init_spi_rx2";
    case 64: return "drv_init_spi_rx3";
    case 65: return "drv_init_spi_rx4";
    case 66: return "drv_init_spi_rx5";
    case 67: return "drv_enable_verify_ok";
    case 68: return "drv_enable_fsr1_fsr2";
    case 69: return "drv_enable_dcr_csacr";
    case 70: return "drv_enable_ocpcr";
    case 71: return "drv_enable_spi_rx_fsr1";
    case 72: return "drv_enable_spi_rx_fsr2";
    case 73: return "drv_enable_spi_rx_dcr";
    case 74: return "drv_enable_gpioa_ctl1";
    case 75: return "drv_enable_gpioa_octl";
    case 76: return "drv_enable_gpioa_istat";
    case 77: return "drv_enable_gpiob_ctl1";
    case 78: return "drv_enable_gpiob_octl";
    case 79: return "drv_enable_gpiob_istat";
    case 80: return "drv_enable_spi_stat";
    case 81: return "drv_enable_nfault_edge";
    case 82: return "runtime_fsm_state";
    case 83: return "runtime_gate_flags";
    case 84: return "runtime_i_max_mA";
    case 85: return "runtime_iq_des_mA";
    case 86: return "runtime_iq_filt_mA";
    case 87: return "runtime_phase_iab_mA";
    case 88: return "runtime_pwm_ch0_ch1";
    case 89: return "runtime_pwm_ch2_period";
    case 90: return "runtime_p_des_mrad";
    case 91: return "runtime_v_des_mrad_s";
    case 92: return "runtime_kp_milli";
    case 93: return "runtime_kd_milli";
    case 94: return "runtime_tff_milliNm";
    case 95: return "cal_fsm_status";
    case 96: return "cal_phase_pairs_samples";
    case 97: return "cal_e_zero";
    case 98: return "cal_current_mA";
    case 100: return "cal_theta_start_mrad";
    case 101: return "cal_theta_end_mrad";
    case 102: return "cal_angle_delta_mrad";
    case 103: return "cal_i_d_des_mA";
    case 104: return "cal_i_d_mA";
    case 105: return "cal_i_q_mA";
    case 106: return "cal_v_d_mV";
    case 107: return "cal_v_q_mV";
    case 108: return "cal_dtc_u_v";
    case 109: return "cal_dtc_w";
    case 110: return "cal_theta_ref_mrad";
    case 111: return "boot_ppairs_milli";
    case 112: return "boot_phase_order_runtime_ppairs";
    case 113: return "boot_e_zero";
    case 114: return "boot_config_crc32";
    case 115: return "boot_encoder_lut_checksum";
    case 116: return "adc_raw_bc";
    case 117: return "adc_offset_bc";
    case 118: return "adc_delta_bc";
    case 119: return "adc_i_scale_uA_count";
    case 120: return "runtime_phase_ibc_mA";
    case 121: return "adc_valid_sample_count";
    case 122: return "adc_timeout_count";
    case 123: return "fault_timestamp_ms";
    case 124: return "fault_fsr1_fsr2";
    case 125: return "fault_adc_b_c";
    case 126: return "fault_adc_offsets_b_c";
    case 127: return "fault_vbus_adc_raw";
    case 128: return "fault_gpioa_istat";
    case 129: return "fault_gpioa_octl";
    case 130: return "fault_iq_des_mA";
    case 131: return "fault_iq_mA";
    case 132: return "fault_id_mA";
    case 133: return "fault_iq_filt_mA";
    case 134: return "fault_vbus_filt_mV";
    case 135: return "fault_duty_u_v_x10000";
    case 136: return "fault_duty_w_x10000";
    case 99: return "debug_status";
    default: return "unknown_snapshot";
    }
}

static const char *counter_name(uint8_t page)
{
    switch (page) {
    case 9: return "spi_timeout";
    case 10: return "adc_timeout";
    case 11: return "encoder_parity";
    case 12: return "encoder_ef";
    case 13: return "encoder_jump";
    default: return "unknown_counter";
    }
}

static void print_safety_flags(uint32_t flags)
{
    unsigned unsafe = (unsigned)((flags >> 1) & 1U) |
                      (unsigned)((flags >> 2) & 1U) |
                      (unsigned)((flags >> 8) & 1U) |
                      (unsigned)((flags >> 9) & 1U) |
                      (unsigned)((flags >> 10) & 1U);
    printf("  SAFE=%u nFAULT=%u PA11=%u POEN=%u CH=%u%u%u enc=%u adc=%u can(W/P/B)=%u%u%u passive=%s\n",
           (unsigned)(flags >> 31), (unsigned)(flags & 1U),
           (unsigned)((flags >> 1) & 1U), (unsigned)((flags >> 2) & 1U),
           (unsigned)((flags >> 8) & 1U), (unsigned)((flags >> 9) & 1U),
           (unsigned)((flags >> 10) & 1U), (unsigned)((flags >> 3) & 1U),
           (unsigned)((flags >> 4) & 1U), (unsigned)((flags >> 5) & 1U),
           (unsigned)((flags >> 6) & 1U), (unsigned)((flags >> 7) & 1U),
           ((flags >> 31) == 1U && (!unsafe ||
             ((flags & (1U << 1)) == 0U &&
              (flags & (0x7U << 8)) == (0x7U << 8)))) ? "PASS" : "FAIL");
}

static int passive_flags_ok(uint32_t flags)
{
    const uint32_t forbidden = (1U << 1) | (1U << 2) |
                               (1U << 8) | (1U << 9) | (1U << 10);
    return (flags & (1U << 31)) != 0U && (flags & forbidden) == 0U;
}

static int inject_passive_flags_ok(uint32_t flags)
{
    const uint32_t required = (0x7U << 8);
    return (flags & (1U << 31)) != 0U &&
           (flags & 1U) == 0U &&
           (flags & (1U << 1)) == 0U &&
           (flags & required) == required &&
           (flags & (1U << 3)) != 0U &&
           (flags & (1U << 4)) != 0U;
}

static int run_passive_safety_gate(struct client *client)
{
    struct response response, info;
    int is_inject = 0;
    if (query(client, OPCODE_SNAPSHOT, 3, &response) != 0) return -1;
    print_response(&response);
    if (query(client, OPCODE_INFO, 2, &info) == 0 && info.status == 0U &&
        (info.payload & (1U << 16)) != 0U) {
        is_inject = 1;
    }
    if (response.status != 0U ||
        (is_inject ? !inject_passive_flags_ok(response.payload)
                   : !passive_flags_ok(response.payload))) {
        fprintf(stderr,
                "Refusing continued diagnostics: PA11/TIMER0/PWM safe-state check failed.\n");
        return -1;
    }
    return 0;
}

static int run_inject_safety_gate(struct client *client)
{
    struct response response;
    uint32_t flags;
    if (query(client, OPCODE_SNAPSHOT, 3, &response) != 0) return -1;
    print_response(&response);
    if (response.status != 0U) {
        fprintf(stderr, "Inject preflight failed: snapshot status %u.\n",
                response.status);
        return -1;
    }
    flags = response.payload;
    if ((flags & (1U << 31)) == 0U) {
        fprintf(stderr, "Inject preflight failed: not a BRINGUP_INJECT profile.\n");
        return -1;
    }
    if ((flags & (1U << 1)) != 0U) {
        fprintf(stderr, "Inject preflight failed: PA11 is high.\n");
        return -1;
    }
    if ((flags & 1U) != 0U) {
        fprintf(stderr, "Inject preflight failed: nFAULT is asserted.\n");
        return -1;
    }
    if ((flags & (1U << 3)) == 0U || (flags & (1U << 4)) == 0U) {
        fprintf(stderr, "Inject preflight failed: encoder or ADC not valid.\n");
        return -1;
    }
    return 0;
}

static void print_response(const struct response *response)
{
    const char *name = response->opcode == OPCODE_SNAPSHOT
                           ? snapshot_name(response->page)
                           : response->opcode == OPCODE_COUNTER
                                 ? counter_name(response->page)
                           : response->opcode == (OPCODE_CONTROL | 0x80U)
                                 ? "control" : "info";
    printf("op=%u page=%u %-24s status=%-11s payload=0x%08X (%u)\n",
           response->opcode, response->page, name,
           status_name(response->status), response->payload,
           response->payload);
    if (response->opcode == OPCODE_SNAPSHOT && response->page == 3U &&
        response->status == 0U) {
        print_safety_flags(response->payload);
    }
    if (response->opcode == (OPCODE_CONTROL | 0x80U) &&
        response->page == 10U && response->status == 0U) {
        printf("  debug timestamp_ms=%u\n", response->payload);
    } else if (response->opcode == (OPCODE_CONTROL | 0x80U) &&
               response->page == 11U && response->status == 0U) {
        static const char *const events[] = {
            "none", "MIT_RX", "ENABLE", "DISABLE", "ZERO",
            "WATCHDOG_TIMEOUT", "GATE_PREFLIGHT", "FSM", "DRV_FAULT",
            "DEBUG_CONTROL", "CALIBRATION_FAIL"
        };
        printf("  debug event=%u (%s)\n", response->payload,
               response->payload < sizeof(events) / sizeof(events[0])
                   ? events[response->payload] : "UNKNOWN");
    } else if (response->opcode == (OPCODE_CONTROL | 0x80U) &&
               response->page == 12U && response->status == 0U) {
        printf("  debug payload=0x%08X\n", response->payload);
    }
    if (response->opcode == OPCODE_SNAPSHOT && response->page == 49U &&
        response->status == 0U) {
        const char *reason = "unknown";
        switch (response->payload) {
        case 1U: reason = "boot/reset"; break;
        case 2U: reason = "new drv-wake"; break;
        case 3U: reason = "explicit stop"; break;
        case 4U: reason = "unknown control frame"; break;
        default: break;
        }
        printf("  drv_ready clear reason: %s\n", reason);
    }
    if (response->opcode == OPCODE_SNAPSHOT && response->page == 31U &&
        response->status == 0U) {
        static const char *const labels[] = {"OK", "TBE_TIMEOUT", "RBNE_TIMEOUT", "BUSY_TIMEOUT"};
        unsigned i;
        printf("  SPI transfer status:");
        for (i = 0U; i < 8U; ++i) {
            printf(" %u=%s", i, labels[(response->payload >> (i * 2U)) & 3U]);
        }
        putchar('\n');
    }
    if (response->opcode == OPCODE_SNAPSHOT && response->page >= 32U &&
        response->page <= 39U && response->status == 0U) {
        printf("  SPI tx=0x%04X rx=0x%04X\n", (unsigned)(response->payload & 0xFFFFU),
               (unsigned)(response->payload >> 16));
    }
    if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
        response->page >= 82U && response->page <= 94U) {
        int32_t signed_value = (int32_t)response->payload;
        if (response->page == 82U) {
            printf("  FSM state=%u next=%u ready=%u\n",
                   response->payload & 0xFFU,
                   (response->payload >> 8) & 0xFFU,
                   (response->payload >> 16) & 0xFFU);
        } else if (response->page == 83U) {
            printf("  GATE drv_ready=%u drv_fault=%u adc=%u enc=%u PA11=%u POEN=%u CH=%u%u%u\n",
                   response->payload & 1U, (response->payload >> 1) & 1U,
                   (response->payload >> 2) & 1U, (response->payload >> 3) & 1U,
                   (response->payload >> 4) & 1U, (response->payload >> 5) & 1U,
                   (response->payload >> 8) & 1U, (response->payload >> 7) & 1U,
                   (response->payload >> 6) & 1U);
        } else if (response->page == 87U) {
            printf("  phase_i a=%.3f A b=%.3f A\n",
                   (int16_t)(response->payload & 0xFFFFU) / 1000.0,
                   (int16_t)((response->payload >> 16) & 0xFFFFU) / 1000.0);
        } else if (response->page >= 84U && response->page <= 86U) {
            printf("  %.3f A\n", signed_value / 1000.0);
        } else if (response->page >= 90U && response->page <= 94U) {
            printf("  %.3f\n", signed_value / 1000.0);
        }
    }
    if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
        response->page >= 116U && response->page <= 122U) {
        if (response->page == 116U) {
            printf("  adc_raw B=%u C=%u\n", response->payload & 0xFFFFU,
                   (response->payload >> 16) & 0xFFFFU);
        } else if (response->page == 117U) {
            printf("  adc_offset B=%u C=%u\n", response->payload & 0xFFFFU,
                   (response->payload >> 16) & 0xFFFFU);
        } else if (response->page == 118U) {
            printf("  adc_delta B=%d C=%d counts\n",
                   (int16_t)(response->payload & 0xFFFFU),
                   (int16_t)((response->payload >> 16) & 0xFFFFU));
        } else if (response->page == 119U) {
            printf("  i_scale=%.6f A/count\n", response->payload / 1000000.0);
        } else if (response->page == 120U) {
            printf("  phase_i b=%.3f A c=%.3f A\n",
                   (int16_t)(response->payload & 0xFFFFU) / 1000.0,
                   (int16_t)((response->payload >> 16) & 0xFFFFU) / 1000.0);
        } else if (response->page == 121U) {
            printf("  adc_valid=%u sample_count=%u\n",
                   response->payload & 0xFFU, response->payload >> 8);
        } else {
            printf("  adc_timeout_count=%u\n", response->payload);
        }
    }
    if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
        response->page == 95U) {
        printf("  calibration started=%u done_cal=%u done_ordering=%u\n",
               (response->payload >> 16) & 1U,
               (response->payload >> 25) & 1U,
               (response->payload >> 24) & 1U);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 96U) {
        printf("  phase_order=%u pole_pairs=%u samples=%u\n",
               response->payload & 0xFFU,
               (response->payload >> 8) & 0xFFU,
               (response->payload >> 16) & 0xFFFFU);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page >= 100U && response->page <= 107U) {
        printf("  %.3f\n", (int32_t)response->payload / 1000.0);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 108U) {
        printf("  dtc_u=%.4f dtc_v=%.4f\n",
               (int16_t)(response->payload & 0xFFFFU) / 10000.0,
               (int16_t)((response->payload >> 16) & 0xFFFFU) / 10000.0);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 109U) {
        printf("  dtc_w=%.4f\n", (int16_t)(response->payload & 0xFFFFU) / 10000.0);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 110U) {
        printf("  %.3f\n", (int32_t)response->payload / 1000.0);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 97U) {
        printf("  E_ZERO=%d\n", (int32_t)response->payload);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 98U) {
        printf("  I_CAL=%.1f A\n", response->payload / 1000.0);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 111U) {
        printf("  PPAIRS=%.3f\n", (int32_t)response->payload / 1000.0);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 112U) {
        printf("  PHASE_ORDER=%u comm_encoder.ppairs=%u\n",
               response->payload & 0xFFFFU, (response->payload >> 16) & 0xFFFFU);
    } else if (response->opcode == OPCODE_SNAPSHOT && response->status == 0U &&
               response->page == 113U) {
        printf("  E_ZERO=%d\n", (int32_t)response->payload);
    }
}

static int run_ping(struct client *client)
{
    struct response response;
    if (query(client, OPCODE_PING, 0, &response) != 0) return -1;
    print_response(&response);
    if (response.status != 0U || response.payload != 0x4E485441U) {
        fprintf(stderr, "PING identity is not ATHN.\n");
        return -1;
    }
    return 0;
}

static int run_pages(struct client *client, uint8_t opcode,
                     const uint8_t *pages, size_t count)
{
    size_t i;
    for (i = 0; i < count && keep_running; ++i) {
        struct response response;
        if (query(client, opcode, pages[i], &response) != 0) return -1;
        print_response(&response);
        if (i + 1U < count) sleep_ms(client->options.interval_ms);
    }
    return 0;
}

static int run_info(struct client *client)
{
    static const uint8_t pages[] = {0, 1, 2, 3, 4};
    return run_pages(client, OPCODE_INFO, pages, sizeof(pages));
}

static int run_snapshot(struct client *client)
{
    if (run_pages(client, OPCODE_SNAPSHOT, snapshot_pages,
                  sizeof(snapshot_pages)) != 0) return -1;
    return run_pages(client, OPCODE_COUNTER, counter_pages,
                     sizeof(counter_pages));
}

static int find_table_entry(double value, const double *table, size_t count,
                            size_t *index)
{
    size_t i;
    for (i = 0; i < count; ++i) {
        if (fabs(value - table[i]) < 1e-9) {
            *index = i;
            return 0;
        }
    }
    return -1;
}

static const char *inject_result_name(uint32_t result)
{
    switch (result) {
    case 0: return "NONE";
    case INJECT_RESULT_OK: return "OK";
    case INJECT_RESULT_ABORTED: return "ABORTED";
    case INJECT_RESULT_TIMEOUT: return "TIMEOUT";
    case INJECT_RESULT_FAULT: return "FAULT";
    case INJECT_RESULT_CURRENT_LIMIT: return "CURRENT_LIMIT";
    default: return "INVALID";
    }
}

static int run_inject(struct client *client, unsigned vector, double duty,
                      unsigned duration)
{
    struct response response;
    size_t duty_idx;
    size_t duration_idx;
    uint8_t request[8];
    uint8_t sequence;
    uint32_t status_word;
    uint32_t result;
    uint64_t deadline;

    if (vector >= INJECT_VECTOR_COUNT) {
        fprintf(stderr, "Vector must be 0..%u.\n", INJECT_VECTOR_COUNT - 1U);
        return -1;
    }
    if (find_table_entry(duty, inject_duty_percent,
                         sizeof(inject_duty_percent) /
                             sizeof(inject_duty_percent[0]),
                         &duty_idx) != 0) {
        fprintf(stderr, "Duty must be one of: 0.5 1.0 1.5 2.0 2.5 3.0 3.5 4.0 4.5 5.0 (percent).\n");
        return -1;
    }
    if (find_table_entry((double)duration, inject_duration_ms,
                         sizeof(inject_duration_ms) /
                             sizeof(inject_duration_ms[0]),
                         &duration_idx) != 0) {
        fprintf(stderr, "Duration must be one of: 10 20 30 40 50 (ms).\n");
        return -1;
    }
    if (!client->options.confirm_inject) {
        fprintf(stderr,
                "Refusing: pass --confirm-inject to enable the gated single-phase pulse.\n");
        return -1;
    }
    /* DRV_WAKE is implemented by the normal image as a bounded, PWM-off
     * probe.  Do not apply the BRINGUP_INJECT profile preflight here: normal
     * firmware intentionally has a different safety profile and would be
     * rejected before the opcode could be sent. */

    sequence = ++client->sequence;
    build_inject_request(OPCODE_INJECT, sequence, (uint8_t)vector,
                         (uint8_t)duty_idx, (uint8_t)duration_idx, request);
    printf("Inject vector=%u duty=%.1f%% duration=%ums\n", vector, duty, duration);
    if (query_raw(client, request, OPCODE_INJECT, (uint8_t)vector,
                  &response) != 0) {
        return -1;
    }
    print_response(&response);
    if (response.status != 0U) {
        fprintf(stderr, "Inject refused by firmware (status %u).\n",
                response.status);
        return -1;
    }

    sleep_ms(duration + 250U);
    deadline = monotonic_ms() + 3000U;
    do {
        if (query(client, OPCODE_SNAPSHOT, 20, &response) != 0) return -1;
        print_response(&response);
        if ((response.payload & 0xFU) != 1U) break; /* not ACTIVE */
        sleep_ms(50U);
    } while (monotonic_ms() < deadline);
    status_word = response.payload;

    if (query(client, OPCODE_SNAPSHOT, 21, &response) != 0) return -1;
    print_response(&response);
    if (response.status == 0U) {
        int16_t peak_b = (int16_t)(response.payload & 0xFFFFU);
        int16_t peak_c = (int16_t)(response.payload >> 16);
        printf("Signed peak ADC deviation: B=%d C=%d (raw counts)\n",
               (int)peak_b, (int)peak_c);
    }
    if (run_pages(client, OPCODE_SNAPSHOT,
                  inject_result_pages + 2,
                  sizeof(inject_result_pages) - 2U) != 0) {
        return -1;
    }
    result = (status_word >> 4) & 0xFU;
    printf("Inject result: %s\n", inject_result_name(result));
    return result == INJECT_RESULT_OK ? 0 : -1;
}

static int run_stop(struct client *client)
{
    struct response response;
    uint8_t request[8];
    uint8_t sequence = ++client->sequence;

    build_inject_request(OPCODE_STOP, sequence, 0U, 0U, 0U, request);
    if (query_raw(client, request, OPCODE_STOP, 0U, &response) != 0) return -1;
    print_response(&response);
    if (response.status != 0U) return -1;
    if (query(client, OPCODE_SNAPSHOT, 20, &response) != 0) return -1;
    print_response(&response);
    return 0;
}

static int run_drv(struct client *client)
{
    return run_pages(client, OPCODE_SNAPSHOT, drv_pages, sizeof(drv_pages));
}

static int run_drv_wake(struct client *client)
{
    struct response response;
    int verification_failed;

    if (!client->options.confirm_drv_wake) {
        fprintf(stderr,
                "Refusing: pass --confirm-drv-wake for the bounded PA11 DRV SPI probe.\n");
        return -1;
    }
    /* Status pages are read-only and are valid on the normal image; the
     * inject-only preflight would incorrectly reject an otherwise healthy
     * normal target. */
    if (query(client, OPCODE_DRV_WAKE, 0U, &response) != 0) return -1;
    print_response(&response);
    /* Normal-image drv-wake returns packed FSR1/FSR2 in the payload; the
     * BRINGUP_INJECT image historically returned a boolean. */
    verification_failed = response.status != 0U;
    /* The wake request may fail legitimately. Its latched pages are read-only
     * evidence and must be shown before the command reports that failure. */
    if (run_pages(client, OPCODE_SNAPSHOT, drv_wake_pages,
                  sizeof(drv_wake_pages)) != 0) {
        return -1;
    }
    if (verification_failed) {
        fprintf(stderr, "DRV wake/configuration verification failed.\n");
        return -1;
    }
    return 0;
}

static int run_drv_wake_status(struct client *client)
{
    /* Read-only wake evidence is also exposed by the normal image; no
     * BRINGUP_INJECT preflight is required. */
    return run_pages(client, OPCODE_SNAPSHOT, drv_wake_pages,
                     sizeof(drv_wake_pages));
}

static int run_normal_drv_status(struct client *client)
{
    /* Normal firmware exposes read-only pages; the inject preflight is not
     * applicable because it intentionally rejects an enabled app. */
    return run_pages(client, OPCODE_SNAPSHOT, normal_drv_status_pages,
                     sizeof(normal_drv_status_pages));
}

static int run_control(struct client *client, const char *name, double value,
                       int has_value)
{
    uint8_t page;
    uint8_t argument = 0U;
    int debug_log = 0;
    struct response response;
    if (!strcmp(name, "esc")) page = 1U;
    else if (!strcmp(name, "motor")) page = 2U;
    else if (!strcmp(name, "encoder")) page = 3U;
    else if (!strcmp(name, "calibrate")) {
        if (!has_value || !isfinite(value) || value < 0.1 || value > 2.0) {
            fprintf(stderr, "Calibration current must be 0.1..2.0 A.\n");
            return -1;
        }
        page = 4U;
        argument = (uint8_t)lround(value * 10.0);
    } else if (!strcmp(name, "zero")) page = 5U;
    else if (!strcmp(name, "abort")) page = 6U;
    else if (!strcmp(name, "debug-on")) page = 7U;
    else if (!strcmp(name, "debug-off")) page = 8U;
    else if (!strcmp(name, "debug-clear")) page = 9U;
    else if (!strcmp(name, "debug-status")) {
        if (query(client, OPCODE_SNAPSHOT, 99U, &response) != 0) return -1;
        print_response(&response);
        return response.status == 0U ? 0 : -1;
    }
    else if (!strcmp(name, "debug-log")) {
        if (!has_value || !isfinite(value) || value < 0.0 || value > 31.0 ||
            lround(value) != value) {
            fprintf(stderr, "Debug log index must be an integer 0..31.\n");
            return -1;
        }
        page = 10U;
        argument = (uint8_t)lround(value);
        debug_log = 1;
    }
    else {
        fprintf(stderr, "Unknown control '%s'.\n", name);
        return -1;
    }
    if (debug_log) {
        uint8_t field;
        for (field = 0U; field < 3U; ++field) {
            if (control(client, (uint8_t)(10U + field), argument, &response) != 0) return -1;
            print_response(&response);
            if (response.status != 0U) return -1;
            /* The UC12 firmware rate-limits diagnostic control frames.  A
             * debug-log entry is three consecutive control requests; without
             * the normal inter-request spacing the second/third request can
             * be dropped and look like a missing log record. */
            if (field < 2U) sleep_ms(client->options.interval_ms);
        }
        return 0;
    }
    if (control(client, page, argument, &response) != 0) return -1;
    print_response(&response);
    return response.status == 0U ? 0 : -1;
}

static FILE *open_csv(const char *path)
{
    FILE *csv = fopen(path, "w");
    if (!csv) {
        fprintf(stderr, "Cannot open CSV %s: %s\n", path, strerror(errno));
        return NULL;
    }
    fprintf(csv, "host_epoch_us,elapsed_ms,sequence,opcode,page,name,status,payload_hex,payload_u32,supply_volts,current_limit_amps\n");
    fflush(csv);
    return csv;
}

static void write_csv(FILE *csv, const struct options *options,
                      uint64_t started_ms, const struct response *response)
{
    fprintf(csv, "%llu,%llu,%u,%u,%u,%s,%u,0x%08X,%u,",
            (unsigned long long)realtime_us(),
            (unsigned long long)(monotonic_ms() - started_ms),
            response->sequence, response->opcode, response->page,
            response->opcode == OPCODE_SNAPSHOT ? snapshot_name(response->page)
                                                : counter_name(response->page),
            response->status, response->payload, response->payload);
    if (options->have_supply_volts) fprintf(csv, "%.4f", options->supply_volts);
    fputc(',', csv);
    if (options->have_current_limit)
        fprintf(csv, "%.4f", options->current_limit_amps);
    fputc('\n', csv);
    fflush(csv);
}

static int run_watch(struct client *client, int require_csv)
{
    struct query_page queries[sizeof(snapshot_pages) + sizeof(counter_pages)];
    char generated_path[96];
    const char *csv_path = client->options.csv_path;
    FILE *csv = NULL;
    size_t count = 0;
    size_t index = 0;
    size_t i;
    uint64_t started_ms = monotonic_ms();
    uint64_t deadline = started_ms + (uint64_t)client->options.seconds * 1000U;

    for (i = 0; i < sizeof(snapshot_pages); ++i) {
        queries[count].opcode = OPCODE_SNAPSHOT;
        queries[count++].page = snapshot_pages[i];
    }
    for (i = 0; i < sizeof(counter_pages); ++i) {
        queries[count].opcode = OPCODE_COUNTER;
        queries[count++].page = counter_pages[i];
    }
    if (require_csv && !csv_path) {
        snprintf(generated_path, sizeof(generated_path),
                 "athena_diag_%llu.csv", (unsigned long long)time(NULL));
        csv_path = generated_path;
    }
    if (csv_path) {
        csv = open_csv(csv_path);
        if (!csv) return -1;
        printf("CSV: %s\n", csv_path);
    }
    while (keep_running && monotonic_ms() < deadline) {
        struct response response;
        if (query(client, queries[index].opcode, queries[index].page,
                  &response) != 0) {
            if (csv) fclose(csv);
            return -1;
        }
        if (csv) write_csv(csv, &client->options, started_ms, &response);
        if ((response.opcode == OPCODE_SNAPSHOT && response.page == 3U) ||
            (response.opcode == OPCODE_SNAPSHOT && response.page == 5U) ||
            response.opcode == OPCODE_COUNTER) {
            print_response(&response);
        }
        index = (index + 1U) % count;
        sleep_ms(client->options.interval_ms);
    }
    if (csv) fclose(csv);
    return keep_running ? 0 : 130;
}

static int self_test(void)
{
    static const uint8_t ping_expected[8] =
        {0xA5, 0x5A, 0x01, 0x00, 0x01, 0x00, 0x00, 0x7D};
    static const uint8_t info_expected[8] =
        {0xA5, 0x5A, 0x01, 0x01, 0x02, 0x00, 0x00, 0xD6};
    static const uint8_t snap_expected[8] =
        {0xA5, 0x5A, 0x01, 0x02, 0x03, 0x03, 0x00, 0xB8};
    static const uint8_t counter_expected[8] =
        {0xA5, 0x5A, 0x01, 0x03, 0x04, 0x00, 0x00, 0x87};
    uint8_t request[8];
    uint8_t record[UC12_RECORD_SIZE] = {0};
    struct response response;

    build_request(OPCODE_PING, 1, 0, request);
    if (memcmp(request, ping_expected, 8) != 0) return 1;
    build_request(OPCODE_INFO, 2, 0, request);
    if (memcmp(request, info_expected, 8) != 0) return 1;
    build_request(OPCODE_SNAPSHOT, 3, 3, request);
    if (memcmp(request, snap_expected, 8) != 0) return 1;
    build_request(OPCODE_COUNTER, 4, 0, request);
    if (memcmp(request, counter_expected, 8) != 0) return 1;

    build_inject_request(OPCODE_INJECT, 5, 0, 0, 0, request);
    if (request[3] != OPCODE_INJECT || request[5] != 0U ||
        request[6] != 0U || crc8_atm(request, 7) != request[7]) return 1;
    build_inject_request(OPCODE_INJECT, 6, 5, 9, 4, request);
    if (request[5] != 5U || request[6] != 0x49U ||
        crc8_atm(request, 7) != request[7]) return 1;
    build_inject_request(OPCODE_STOP, 7, 0, 0, 0, request);
    if (request[3] != OPCODE_STOP || request[5] != 0U ||
        request[6] != 0U || crc8_atm(request, 7) != request[7]) return 1;

    record[6] = 8;
    write_le32(record + 7, DIAG_RESPONSE_ID << 5);
    record[11] = OPCODE_SNAPSHOT | 0x80U;
    record[12] = 3;
    record[13] = 3;
    record[14] = 0;
    write_le32(record + 15, 0x80000000U);
    if (!parse_response_record(record, 0, OPCODE_SNAPSHOT, 3, 3, &response) ||
        response.payload != 0x80000000U) return 1;
    if (!passive_flags_ok(response.payload) ||
        passive_flags_ok(response.payload | (1U << 1)) ||
        passive_flags_ok(0U)) return 1;
    record[6] |= 0x80U;
    if (parse_response_record(record, 0, OPCODE_SNAPSHOT, 3, 3, &response)) return 1;
    record[6] &= 0x7FU;
    record[6] = 7;
    if (parse_response_record(record, 0, OPCODE_SNAPSHOT, 3, 3, &response)) return 1;

    record[6] = 8;
    write_le32(record + 7, DIAG_RESPONSE_ID << 5);
    record[11] = OPCODE_INJECT | 0x80U;
    record[12] = 5;
    record[13] = 0;
    record[14] = 0;
    write_le32(record + 15, 0x00000000U);
    if (!parse_response_record(record, 0, OPCODE_INJECT, 5, 0, &response) ||
        response.payload != 0U) return 1;

    puts("PASS: ATHENA-DIAG golden requests, INJECT/STOP encoding, strict standard/DLC8 response filtering, and response decoding.");
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [OPTIONS] COMMAND\n"
            "Commands: ping, info, snapshot, watch, export\n"
            "          inject VECTOR DUTY_PCT DURATION_MS, stop, drv, drv-wake,\n"
            "          drv-wake-status\n"
            "          drv-status, control esc|motor|encoder|zero|abort|debug-on|debug-off|debug-clear|debug-status\n"
            "          control calibrate CURRENT_A, control debug-log INDEX\n"
            "Options:\n"
            "  --channel 0|1              UC12 CAN channel (default 0)\n"
            "  --timeout-ms N             response timeout (default 1000)\n"
            "  --interval-ms N            request interval, minimum 25 (default 25)\n"
            "  --seconds N                watch/export duration (default 600)\n"
            "  --csv FILE                 narrow raw response CSV\n"
            "  --supply-volts V           operator-entered value recorded in CSV\n"
            "  --current-limit-amps A     operator-entered value recorded in CSV\n"
            "  --confirm-inject           required to arm the gated injection pulse\n"
            "  --confirm-drv-wake         required for the bounded PA11 DRV SPI probe\n"
            "  --self-test                offline tests; does not open USB\n",
            program);
}

static int parse_unsigned(const char *text, unsigned *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *text == '\0' || *end != '\0' || parsed > 0xFFFFFFFFUL)
        return -1;
    *value = (unsigned)parsed;
    return 0;
}

static int parse_double_value(const char *text, double *value)
{
    char *end;
    errno = 0;
    *value = strtod(text, &end);
    return errno || *text == '\0' || *end != '\0' ? -1 : 0;
}

int main(int argc, char **argv)
{
    static const struct option long_options[] = {
        {"channel", required_argument, NULL, 'c'},
        {"timeout-ms", required_argument, NULL, 't'},
        {"interval-ms", required_argument, NULL, 'i'},
        {"seconds", required_argument, NULL, 's'},
        {"csv", required_argument, NULL, 'o'},
        {"supply-volts", required_argument, NULL, 'v'},
        {"current-limit-amps", required_argument, NULL, 'a'},
        {"confirm-inject", no_argument, NULL, 'j'},
        {"confirm-drv-wake", no_argument, NULL, 'w'},
        {"self-test", no_argument, NULL, 'T'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    struct client client;
    const char *command;
    int option;
    int result = 1;
    int do_self_test = 0;
    unsigned inject_vector = 0;
    double inject_duty = 0.0;
    unsigned inject_duration = 0;
    int have_inject_args = 0;
    const char *control_name = NULL;
    double control_value = 0.0;
    int have_control_value = 0;

    memset(&client, 0, sizeof(client));
    client.options.timeout_ms = 1000;
    client.options.interval_ms = 25;
    client.options.seconds = 600;
    while ((option = getopt_long(argc, argv, "c:t:i:s:o:v:a:jwTh",
                                 long_options, NULL)) != -1) {
        switch (option) {
        case 'c':
            if (parse_unsigned(optarg, &client.options.channel) != 0 ||
                client.options.channel > 1U) {
                fprintf(stderr, "Invalid UC12 channel.\n");
                return 2;
            }
            break;
        case 't':
            if (parse_unsigned(optarg, &client.options.timeout_ms) != 0 ||
                client.options.timeout_ms < 50U) return 2;
            break;
        case 'i':
            if (parse_unsigned(optarg, &client.options.interval_ms) != 0 ||
                client.options.interval_ms < 25U) {
                fprintf(stderr, "Request interval must be at least 25 ms.\n");
                return 2;
            }
            break;
        case 's':
            if (parse_unsigned(optarg, &client.options.seconds) != 0 ||
                client.options.seconds == 0U) return 2;
            break;
        case 'o': client.options.csv_path = optarg; break;
        case 'v':
            if (parse_double_value(optarg, &client.options.supply_volts) != 0 ||
                !isfinite(client.options.supply_volts) ||
                client.options.supply_volts <= 0.0) return 2;
            client.options.have_supply_volts = 1;
            break;
        case 'a':
            if (parse_double_value(optarg, &client.options.current_limit_amps) != 0 ||
                !isfinite(client.options.current_limit_amps) ||
                client.options.current_limit_amps <= 0.0) return 2;
            client.options.have_current_limit = 1;
            break;
        case 'j': client.options.confirm_inject = 1; break;
        case 'w': client.options.confirm_drv_wake = 1; break;
        case 'T': do_self_test = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (do_self_test) return self_test();
    if (optind + 1 != argc &&
        !(optind + 4 == argc && !strcmp(argv[optind], "inject")) &&
        !(optind + 2 <= argc && optind + 3 >= argc && !strcmp(argv[optind], "control"))) {
        usage(argv[0]);
        return 2;
    }
    command = argv[optind];
    if (strcmp(command, "ping") && strcmp(command, "info") &&
        strcmp(command, "snapshot") && strcmp(command, "watch") &&
        strcmp(command, "export") && strcmp(command, "inject") &&
        strcmp(command, "stop") && strcmp(command, "drv") &&
        strcmp(command, "drv-wake") && strcmp(command, "drv-wake-status") &&
        strcmp(command, "drv-status") && strcmp(command, "control")) {
        usage(argv[0]);
        return 2;
    }
    if (!strcmp(command, "inject")) {
        if (parse_unsigned(argv[optind + 1], &inject_vector) != 0 ||
            inject_vector >= INJECT_VECTOR_COUNT) {
            fprintf(stderr, "Invalid vector; use 0..%u.\n",
                    INJECT_VECTOR_COUNT - 1U);
            return 2;
        }
        if (parse_double_value(argv[optind + 2], &inject_duty) != 0 ||
            !isfinite(inject_duty) || inject_duty <= 0.0) return 2;
        if (parse_unsigned(argv[optind + 3], &inject_duration) != 0 ||
            inject_duration == 0U) return 2;
        have_inject_args = 1;
    } else if (!strcmp(command, "control")) {
        if (optind + 1 >= argc || optind + 3 < argc) return 2;
        control_name = argv[optind + 1];
        if (!strcmp(control_name, "calibrate") || !strcmp(control_name, "debug-log")) {
            if (optind + 2 >= argc || parse_double_value(argv[optind + 2], &control_value) != 0) return 2;
            have_control_value = 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (open_client(&client) != 0) {
        close_client(&client);
        return 1;
    }
    result = run_ping(&client);
    if (result == 0 && strcmp(command, "ping")) {
        sleep_ms(client.options.interval_ms);
        if (!strcmp(command, "snapshot") || !strcmp(command, "watch") ||
            !strcmp(command, "export")) {
            result = run_passive_safety_gate(&client);
            sleep_ms(client.options.interval_ms);
        }
    }
    if (result == 0 && !strcmp(command, "info")) result = run_info(&client);
    else if (result == 0 && !strcmp(command, "snapshot")) result = run_snapshot(&client);
    else if (result == 0 && !strcmp(command, "watch")) result = run_watch(&client, 0);
    else if (result == 0 && !strcmp(command, "export")) result = run_watch(&client, 1);
    else if (result == 0 && !strcmp(command, "inject") && have_inject_args)
        result = run_inject(&client, inject_vector, inject_duty, inject_duration);
    else if (result == 0 && !strcmp(command, "stop")) result = run_stop(&client);
    else if (result == 0 && !strcmp(command, "drv")) result = run_drv(&client);
    else if (result == 0 && !strcmp(command, "drv-wake")) result = run_drv_wake(&client);
    else if (result == 0 && !strcmp(command, "drv-wake-status"))
        result = run_drv_wake_status(&client);
    else if (result == 0 && !strcmp(command, "drv-status"))
        result = run_normal_drv_status(&client);
    else if (result == 0 && !strcmp(command, "control"))
        result = run_control(&client, control_name, control_value, have_control_value);
    close_client(&client);
    return result == 0 ? 0 : 1;
}
