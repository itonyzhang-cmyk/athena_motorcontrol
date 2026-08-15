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
    0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19
};

static const uint8_t counter_pages[] = {9, 10, 11, 12, 13};

static const uint8_t inject_result_pages[] = {20, 21, 22, 23};
static const uint8_t drv_pages[] = {24, 25, 26};

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
    if (rc != LIBUSB_SUCCESS || transferred < 3 ||
        !(confirmation[2] & 0x80U) || (confirmation[2] & 0x7FU) < 1U) {
        fprintf(stderr, "UC12 did not confirm the diagnostic request.\n");
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
           ((flags >> 31) == 1U && !unsafe) ? "PASS" : "FAIL");
}

static int passive_flags_ok(uint32_t flags)
{
    const uint32_t forbidden = (1U << 1) | (1U << 2) |
                               (1U << 8) | (1U << 9) | (1U << 10);
    return (flags & (1U << 31)) != 0U && (flags & forbidden) == 0U;
}

static int run_passive_safety_gate(struct client *client)
{
    struct response response;
    if (query(client, OPCODE_SNAPSHOT, 3, &response) != 0) return -1;
    print_response(&response);
    if (response.status != 0U || !passive_flags_ok(response.payload)) {
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
                                 : "info";
    printf("op=%u page=%u %-24s status=%-11s payload=0x%08X (%u)\n",
           response->opcode, response->page, name,
           status_name(response->status), response->payload,
           response->payload);
    if (response->opcode == OPCODE_SNAPSHOT && response->page == 3U &&
        response->status == 0U) {
        print_safety_flags(response->payload);
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
    if (run_inject_safety_gate(client) != 0) return -1;

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

    if (run_pages(client, OPCODE_SNAPSHOT,
                  inject_result_pages + 1,
                  sizeof(inject_result_pages) - 1U) != 0) {
        return -1;
    }
    result = (response.payload >> 4) & 0xFU;
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
            "          inject VECTOR DUTY_PCT DURATION_MS, stop, drv\n"
            "Options:\n"
            "  --channel 0|1              UC12 CAN channel (default 0)\n"
            "  --timeout-ms N             response timeout (default 1000)\n"
            "  --interval-ms N            request interval, minimum 25 (default 25)\n"
            "  --seconds N                watch/export duration (default 600)\n"
            "  --csv FILE                 narrow raw response CSV\n"
            "  --supply-volts V           operator-entered value recorded in CSV\n"
            "  --current-limit-amps A     operator-entered value recorded in CSV\n"
            "  --confirm-inject           required to arm the gated injection pulse\n"
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

    memset(&client, 0, sizeof(client));
    client.options.timeout_ms = 1000;
    client.options.interval_ms = 25;
    client.options.seconds = 600;
    while ((option = getopt_long(argc, argv, "c:t:i:s:o:v:a:jTh",
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
        case 'T': do_self_test = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (do_self_test) return self_test();
    if (optind + 1 != argc &&
        !(optind + 4 == argc && !strcmp(argv[optind], "inject"))) {
        usage(argv[0]);
        return 2;
    }
    command = argv[optind];
    if (strcmp(command, "ping") && strcmp(command, "info") &&
        strcmp(command, "snapshot") && strcmp(command, "watch") &&
        strcmp(command, "export") && strcmp(command, "inject") &&
        strcmp(command, "stop") && strcmp(command, "drv")) {
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
    close_client(&client);
    return result == 0 ? 0 : 1;
}
