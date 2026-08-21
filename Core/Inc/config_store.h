#ifndef INC_CONFIG_STORE_H_
#define INC_CONFIG_STORE_H_

#include <stdbool.h>
#include <stdint.h>

#define CONFIG_INT_WORDS 256U
#define CONFIG_FLOAT_WORDS 64U
#define CONFIG_PAYLOAD_WORDS (CONFIG_INT_WORDS + CONFIG_FLOAT_WORDS)

#define CONFIG_METADATA_MAGIC_INDEX 320U
#define CONFIG_METADATA_VERSION_INDEX 321U
#define CONFIG_METADATA_LENGTH_INDEX 322U
#define CONFIG_METADATA_CRC_INDEX 323U
#define CONFIG_METADATA_SEQUENCE_INDEX 324U

#define CONFIG_METADATA_MAGIC 0x4E485441U
#define CONFIG_FORMAT_VERSION 2U

void config_apply_defaults(int int_regs[CONFIG_INT_WORDS],
                           float float_regs[CONFIG_FLOAT_WORDS]);
uint32_t config_payload_crc32(const int int_regs[CONFIG_INT_WORDS],
                              const float float_regs[CONFIG_FLOAT_WORDS]);
bool config_payload_valid(const int int_regs[CONFIG_INT_WORDS],
                          const float float_regs[CONFIG_FLOAT_WORDS]);
bool config_metadata_valid(uint32_t magic, uint32_t version,
                           uint32_t length, uint32_t stored_crc,
                           const int int_regs[CONFIG_INT_WORDS],
                           const float float_regs[CONFIG_FLOAT_WORDS]);
bool config_sequence_newer(uint32_t candidate, uint32_t current);
int config_select_slot(bool slot0_valid, uint32_t slot0_sequence,
                       bool slot1_valid, uint32_t slot1_sequence);

#endif /* INC_CONFIG_STORE_H_ */
