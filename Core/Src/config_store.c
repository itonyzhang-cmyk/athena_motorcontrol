#include "config_store.h"
#include "diag_protocol.h"

#include <math.h>
#include <string.h>

enum {
    CONFIG_PHASE_ORDER = 0,
    CONFIG_CAN_ID = 1,
    CONFIG_CAN_MASTER = 2,
    CONFIG_CAN_TIMEOUT = 3,
    CONFIG_M_ZERO = 4,
    CONFIG_E_ZERO = 5,
    CONFIG_IVT_PROTECT_ENABLE = 7
};

enum {
    CONFIG_I_BW = 2,
    CONFIG_I_MAX = 3,
    CONFIG_I_FW_MAX = 6,
    CONFIG_R_NOMINAL = 7,
    CONFIG_TEMP_MAX = 8,
    CONFIG_I_MAX_CONT = 9,
    CONFIG_PPAIRS = 10,
    CONFIG_R_PHASE = 13,
    CONFIG_KT = 14,
    CONFIG_R_TH = 15,
    CONFIG_C_TH = 16,
    CONFIG_GR = 17,
    CONFIG_I_CAL = 18,
    CONFIG_P_MIN = 19,
    CONFIG_P_MAX = 20,
    CONFIG_V_MIN = 21,
    CONFIG_V_MAX = 22,
    CONFIG_KP_MAX = 23,
    CONFIG_KD_MAX = 24,
    CONFIG_I_TRIP = 25,
    CONFIG_VBUS_MIN = 26,
    CONFIG_VBUS_MAX = 27,
    CONFIG_TEMP_TRIP = 28
};

static bool finite_range(float value, float minimum, float maximum)
{
    return isfinite(value) && value >= minimum && value <= maximum;
}

void config_apply_defaults(int int_regs[CONFIG_INT_WORDS],
                           float float_regs[CONFIG_FLOAT_WORDS])
{
    memset(int_regs, 0, CONFIG_INT_WORDS * sizeof(int_regs[0]));
    memset(float_regs, 0, CONFIG_FLOAT_WORDS * sizeof(float_regs[0]));

    int_regs[CONFIG_PHASE_ORDER] = 0;
    int_regs[CONFIG_CAN_ID] = 1;
    int_regs[CONFIG_CAN_MASTER] = 0;
    int_regs[CONFIG_CAN_TIMEOUT] = 3000;
    int_regs[CONFIG_M_ZERO] = 0;
    int_regs[CONFIG_E_ZERO] = 0;
    /* Deliberately disabled until scales/sensor sources are characterized. */
    int_regs[CONFIG_IVT_PROTECT_ENABLE] = 0;

    float_regs[CONFIG_I_BW] = 1000.0f;
    float_regs[CONFIG_I_MAX] = 40.0f;
    float_regs[CONFIG_I_FW_MAX] = 0.0f;
    float_regs[CONFIG_R_NOMINAL] = 0.0f;
    float_regs[CONFIG_TEMP_MAX] = 125.0f;
    float_regs[CONFIG_I_MAX_CONT] = 14.0f;
    float_regs[CONFIG_PPAIRS] = 21.0f;
    float_regs[CONFIG_R_PHASE] = 0.0f;
    float_regs[CONFIG_KT] = 1.0f;
    float_regs[CONFIG_R_TH] = 0.0f;
    float_regs[CONFIG_C_TH] = 0.0f;
    float_regs[CONFIG_GR] = 1.0f;
    float_regs[CONFIG_I_CAL] = 5.0f;
    float_regs[CONFIG_P_MIN] = -12.5f;
    float_regs[CONFIG_P_MAX] = 12.5f;
    float_regs[CONFIG_V_MIN] = -65.0f;
    float_regs[CONFIG_V_MAX] = 65.0f;
    float_regs[CONFIG_KP_MAX] = 500.0f;
    float_regs[CONFIG_KD_MAX] = 5.0f;
    float_regs[CONFIG_I_TRIP] = 0.0f;
    float_regs[CONFIG_VBUS_MIN] = 0.0f;
    float_regs[CONFIG_VBUS_MAX] = 0.0f;
    float_regs[CONFIG_TEMP_TRIP] = 0.0f;
}

static uint32_t crc32_word(uint32_t crc, uint32_t word)
{
    for (unsigned byte = 0U; byte < 4U; ++byte) {
        crc ^= (word >> (byte * 8U)) & 0xFFU;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

uint32_t config_payload_crc32(const int int_regs[CONFIG_INT_WORDS],
                              const float float_regs[CONFIG_FLOAT_WORDS])
{
    uint32_t crc = 0xFFFFFFFFU;

    for (unsigned i = 0U; i < CONFIG_INT_WORDS; ++i) {
        crc = crc32_word(crc, (uint32_t)int_regs[i]);
    }
    for (unsigned i = 0U; i < CONFIG_FLOAT_WORDS; ++i) {
        uint32_t word;
        memcpy(&word, &float_regs[i], sizeof(word));
        crc = crc32_word(crc, word);
    }
    return ~crc;
}

bool config_payload_valid(const int int_regs[CONFIG_INT_WORDS],
                          const float float_regs[CONFIG_FLOAT_WORDS])
{
    if ((int_regs[CONFIG_PHASE_ORDER] != 0 && int_regs[CONFIG_PHASE_ORDER] != 1) ||
        /* Both fields are 11-bit standard CAN arbitration IDs.  The older
         * 0..127 validation did not match normal_can_frame_matches(), which
         * correctly accepts the complete 11-bit range, and prevented a
         * multi-node bus from assigning a separate feedback-ID block. */
        int_regs[CONFIG_CAN_ID] < 0 || int_regs[CONFIG_CAN_ID] > 0x7FF ||
        int_regs[CONFIG_CAN_MASTER] < 0 || int_regs[CONFIG_CAN_MASTER] > 0x7FF ||
        /* ATHENA-DIAG is fixed-address in this firmware revision.  Do not
         * permit a MIT endpoint to share either address: a multi-node host
         * would otherwise see arbitration/dispatch ambiguity. */
        (int_regs[CONFIG_CAN_ID] == DIAG_CAN_REQUEST_ID ||
         int_regs[CONFIG_CAN_ID] == DIAG_CAN_RESPONSE_ID) ||
        (int_regs[CONFIG_CAN_MASTER] == DIAG_CAN_REQUEST_ID ||
         int_regs[CONFIG_CAN_MASTER] == DIAG_CAN_RESPONSE_ID) ||
        int_regs[CONFIG_CAN_TIMEOUT] < 1 || int_regs[CONFIG_CAN_TIMEOUT] > 100000 ||
        int_regs[CONFIG_M_ZERO] < -65535 || int_regs[CONFIG_M_ZERO] > 65535 ||
        int_regs[CONFIG_E_ZERO] < -65535 || int_regs[CONFIG_E_ZERO] > 65535 ||
        int_regs[CONFIG_IVT_PROTECT_ENABLE] < 0 ||
        int_regs[CONFIG_IVT_PROTECT_ENABLE] > 7) {
        return false;
    }

    if (!finite_range(float_regs[CONFIG_I_BW], 100.0f, 2000.0f) ||
        !finite_range(float_regs[CONFIG_I_MAX], 0.1f, 60.0f) ||
        !finite_range(float_regs[CONFIG_I_FW_MAX], 0.0f, 33.0f) ||
        !finite_range(float_regs[CONFIG_R_NOMINAL], 0.0f, 100.0f) ||
        !finite_range(float_regs[CONFIG_TEMP_MAX], 1.0f, 150.0f) ||
        !finite_range(float_regs[CONFIG_I_MAX_CONT], 0.1f, 40.0f) ||
        !finite_range(float_regs[CONFIG_PPAIRS], 1.0f, 64.0f) ||
        !finite_range(float_regs[CONFIG_R_PHASE], 0.0f, 100.0f) ||
        !finite_range(float_regs[CONFIG_KT], 0.0001f, 10.0f) ||
        !finite_range(float_regs[CONFIG_R_TH], 0.0f, 1000.0f) ||
        !finite_range(float_regs[CONFIG_C_TH], 0.0f, 100000.0f) ||
        !finite_range(float_regs[CONFIG_GR], 0.001f, 1000.0f) ||
        !finite_range(float_regs[CONFIG_I_CAL], 0.1f, 20.0f) ||
        !finite_range(float_regs[CONFIG_P_MIN], -1000.0f, 0.0f) ||
        !finite_range(float_regs[CONFIG_P_MAX], 0.0f, 1000.0f) ||
        !finite_range(float_regs[CONFIG_V_MIN], -1000.0f, 0.0f) ||
        !finite_range(float_regs[CONFIG_V_MAX], 0.0f, 1000.0f) ||
        !finite_range(float_regs[CONFIG_KP_MAX], 0.0f, 1000.0f) ||
        !finite_range(float_regs[CONFIG_KD_MAX], 0.0f, 100.0f) ||
        !finite_range(float_regs[CONFIG_I_TRIP], 0.0f, 60.0f) ||
        !finite_range(float_regs[CONFIG_VBUS_MIN], 0.0f, 200.0f) ||
        !finite_range(float_regs[CONFIG_VBUS_MAX], 0.0f, 200.0f) ||
        !finite_range(float_regs[CONFIG_TEMP_TRIP], 0.0f, 150.0f)) {
        return false;
    }

    return float_regs[CONFIG_I_FW_MAX] <= float_regs[CONFIG_I_MAX] &&
           float_regs[CONFIG_I_MAX_CONT] <= float_regs[CONFIG_I_MAX] &&
           float_regs[CONFIG_P_MIN] < float_regs[CONFIG_P_MAX] &&
           float_regs[CONFIG_V_MIN] < float_regs[CONFIG_V_MAX] &&
           (((int_regs[CONFIG_IVT_PROTECT_ENABLE] & 1) == 0) ||
            (float_regs[CONFIG_I_TRIP] > 0.0f &&
             float_regs[CONFIG_I_TRIP] <= float_regs[CONFIG_I_MAX])) &&
           (((int_regs[CONFIG_IVT_PROTECT_ENABLE] & 2) == 0) ||
            (float_regs[CONFIG_VBUS_MIN] > 0.0f &&
             float_regs[CONFIG_VBUS_MIN] < float_regs[CONFIG_VBUS_MAX])) &&
           (((int_regs[CONFIG_IVT_PROTECT_ENABLE] & 4) == 0) ||
            float_regs[CONFIG_TEMP_TRIP] > 0.0f);
}

bool config_metadata_valid(uint32_t magic, uint32_t version,
                           uint32_t length, uint32_t stored_crc,
                           const int int_regs[CONFIG_INT_WORDS],
                           const float float_regs[CONFIG_FLOAT_WORDS])
{
    return magic == CONFIG_METADATA_MAGIC &&
           version == CONFIG_FORMAT_VERSION &&
           length == CONFIG_PAYLOAD_WORDS &&
           stored_crc == config_payload_crc32(int_regs, float_regs) &&
           config_payload_valid(int_regs, float_regs);
}

bool config_sequence_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

int config_select_slot(bool slot0_valid, uint32_t slot0_sequence,
                       bool slot1_valid, uint32_t slot1_sequence)
{
    if (!slot0_valid) {
        return slot1_valid ? 1 : -1;
    }
    if (!slot1_valid) {
        return 0;
    }
    return config_sequence_newer(slot1_sequence, slot0_sequence) ? 1 : 0;
}
