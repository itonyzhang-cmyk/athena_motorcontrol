#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "config_store.h"

static int int_regs[CONFIG_INT_WORDS];
static float float_regs[CONFIG_FLOAT_WORDS];

void config_store_tests(void)
{
    uint32_t crc;

    config_apply_defaults(int_regs, float_regs);
    assert(config_payload_valid(int_regs, float_regs));
    assert(int_regs[1] == 1);
    assert(int_regs[2] == 0);
    assert(int_regs[3] == 3000);

    crc = config_payload_crc32(int_regs, float_regs);
    assert(config_metadata_valid(CONFIG_METADATA_MAGIC,
                                 CONFIG_FORMAT_VERSION,
                                 CONFIG_PAYLOAD_WORDS, crc,
                                 int_regs, float_regs));
    assert(!config_metadata_valid(0x6C61682DU,
                                  CONFIG_FORMAT_VERSION,
                                  CONFIG_PAYLOAD_WORDS, crc,
                                  int_regs, float_regs));
    assert(!config_metadata_valid(CONFIG_METADATA_MAGIC,
                                  CONFIG_FORMAT_VERSION,
                                  CONFIG_PAYLOAD_WORDS, crc ^ 1U,
                                  int_regs, float_regs));
	assert(config_sequence_newer(2U, 1U));
	assert(!config_sequence_newer(1U, 2U));
	assert(config_sequence_newer(1U, UINT32_MAX));
	assert(config_select_slot(false, 0U, false, 0U) == -1);
	assert(config_select_slot(true, 4U, false, 0U) == 0);
	assert(config_select_slot(false, 0U, true, 7U) == 1);
	assert(config_select_slot(true, 4U, true, 5U) == 1);
	assert(config_select_slot(true, UINT32_MAX, true, 1U) == 1);

    int_regs[1] = 0x000D0A6C;
    assert(!config_payload_valid(int_regs, float_regs));
    config_apply_defaults(int_regs, float_regs);
    float_regs[3] = NAN;
    assert(!config_payload_valid(int_regs, float_regs));
    config_apply_defaults(int_regs, float_regs);
    int_regs[3] = 0;
    assert(!config_payload_valid(int_regs, float_regs));

    puts("config store tests: PASS");
}
