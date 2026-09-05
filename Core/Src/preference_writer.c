/*
 * preference_writer.c
 *
 *  Created on: Apr 13, 2020
 *      Author: ben
 */


#include "preference_writer.h"
#include "flash_writer.h"
#include "user_config.h"
#include "config_store.h"
#include "runtime_watchdog.h"
#include <string.h>

#ifdef STM32F446
#define CONFIG_SLOT_COUNT 1U
#else
#define CONFIG_SLOT_COUNT 2U
#endif
#define CONFIG_SLOT_SIZE 0x800U

static int slot0_int_regs[CONFIG_INT_WORDS];
static float slot0_float_regs[CONFIG_FLOAT_WORDS];

static void preference_watchdog_tick(void)
{
	/* Configuration commits temporarily pause TIMER0, which normally owns the
	 * watchdog service.  Keep the same progress gate in the foreground so a
	 * valid multi-page transaction cannot reset the board mid-write. */
	runtime_watchdog_main_heartbeat();
	runtime_watchdog_timer_service();
}

static FlashWriter preference_slot_writer(PreferenceWriter pr, uint8_t slot)
{
	FlashWriter writer = pr.fw;
	writer.base = pr.slot_base + (uint32_t)slot * CONFIG_SLOT_SIZE;
	return writer;
}

static void preference_read_payload(FlashWriter writer)
{
	int offs;
	for (offs = 0; offs < 256; offs++) {
		preference_watchdog_tick();
		__int_reg[offs] = flash_read_int(writer, offs);
	}
	for (; offs < 320; offs++) {
		preference_watchdog_tick();
		__float_reg[offs - 256] = flash_read_float(writer, offs);
	}
}

static bool preference_slot_valid(FlashWriter writer)
{
	return config_metadata_valid(
		flash_read_uint(writer, CONFIG_METADATA_MAGIC_INDEX),
		flash_read_uint(writer, CONFIG_METADATA_VERSION_INDEX),
		flash_read_uint(writer, CONFIG_METADATA_LENGTH_INDEX),
		flash_read_uint(writer, CONFIG_METADATA_CRC_INDEX),
		__int_reg, __float_reg);
}
/*
PreferenceWriter::PreferenceWriter(uint32_t sector) {
    writer = new FlashWriter(sector);
    __sector = sector;
    __ready = false;
}
*/

void preference_writer_init(PreferenceWriter * pr, uint32_t sector){
	flash_writer_init(&pr->fw, sector);
	pr->slot_base = pr->fw.base;
	pr->active_sequence = 0U;
	pr->active_slot = 0xFFU;
	pr->target_slot = 0U;
#ifdef STM32F446
	pr->sector = sector;
#endif
}


bool preference_writer_open(PreferenceWriter * pr) {
    /* Validate before erase. A rejected update must leave the last committed
     * page intact. */
    if (!config_payload_valid(__int_reg, __float_reg)) {
        pr->ready = false;
        return false;
    }
	pr->target_slot = pr->active_slot < CONFIG_SLOT_COUNT ?
		(uint8_t)((pr->active_slot + 1U) % CONFIG_SLOT_COUNT) : 0U;
	pr->fw = preference_slot_writer(*pr, pr->target_slot);
	preference_watchdog_tick();
	flash_writer_open(&pr->fw);
	preference_watchdog_tick();
	pr->ready = true;
    return true;
}

bool  preference_writer_ready(PreferenceWriter pr) {
    return pr.ready;
}

void preference_writer_write_int(int x, int index) {
    __int_reg[index] = x;
}

void preference_writer_write_float(float x, int index) {
    __float_reg[index] = x;
}

bool preference_writer_flush(PreferenceWriter * pr) {
    int offs;
    uint32_t crc;
	bool committed;

    if (!pr->ready || !config_payload_valid(__int_reg, __float_reg)) {
        pr->ready = false;
        return false;
    }
	crc = config_payload_crc32(__int_reg, __float_reg);
	for (offs = 0; offs < 256; offs++) {
		preference_watchdog_tick();
		flash_writer_write_int(pr->fw, offs, __int_reg[offs]);
	}
	for (; offs < 320; offs++) {
		preference_watchdog_tick();
		flash_writer_write_float(pr->fw, offs, __float_reg[offs - 256]);
    }
    flash_writer_write_uint(pr->fw, CONFIG_METADATA_VERSION_INDEX,
                            CONFIG_FORMAT_VERSION);
    flash_writer_write_uint(pr->fw, CONFIG_METADATA_LENGTH_INDEX,
                            CONFIG_PAYLOAD_WORDS);
    flash_writer_write_uint(pr->fw, CONFIG_METADATA_CRC_INDEX, crc);
	flash_writer_write_uint(pr->fw, CONFIG_METADATA_SEQUENCE_INDEX,
		pr->active_sequence + 1U);
    /* Commit marker is deliberately last. A reset during programming leaves
     * the page invalid instead of exposing a partially-written payload. */
    flash_writer_write_uint(pr->fw, CONFIG_METADATA_MAGIC_INDEX,
                            CONFIG_METADATA_MAGIC);
	/* Validate the programmed slot without allowing a failed readback to
	 * replace the still-valid in-RAM settings. */
	memcpy(slot0_int_regs, __int_reg, sizeof(slot0_int_regs));
	memcpy(slot0_float_regs, __float_reg, sizeof(slot0_float_regs));
	preference_read_payload(pr->fw);
	committed = preference_slot_valid(pr->fw) &&
		flash_read_uint(pr->fw, CONFIG_METADATA_SEQUENCE_INDEX) ==
			pr->active_sequence + 1U;
	memcpy(__int_reg, slot0_int_regs, sizeof(slot0_int_regs));
	memcpy(__float_reg, slot0_float_regs, sizeof(slot0_float_regs));
	if (!committed) {
		pr->ready = false;
		return false;
	}
	pr->active_slot = pr->target_slot;
	pr->active_sequence += 1U;
    pr->ready = false;
    return true;
}

bool preference_writer_load(PreferenceWriter *pr) {
	bool valid[CONFIG_SLOT_COUNT];
	uint32_t sequence[CONFIG_SLOT_COUNT];
	uint8_t selected = 0xFFU;

	for (uint8_t slot = 0U; slot < CONFIG_SLOT_COUNT; ++slot) {
		preference_watchdog_tick();
		FlashWriter writer = preference_slot_writer(*pr, slot);
		preference_read_payload(writer);
		valid[slot] = preference_slot_valid(writer);
		sequence[slot] = flash_read_uint(writer, CONFIG_METADATA_SEQUENCE_INDEX);
		if (slot == 0U && valid[slot]) {
			memcpy(slot0_int_regs, __int_reg, sizeof(slot0_int_regs));
			memcpy(slot0_float_regs, __float_reg, sizeof(slot0_float_regs));
		}
		if (valid[slot] &&
		    (selected == 0xFFU || config_sequence_newer(sequence[slot], sequence[selected]))) {
			selected = slot;
		}
	}
	if (CONFIG_SLOT_COUNT == 2U) {
		int selected_pair = config_select_slot(valid[0], sequence[0],
		                                      valid[1], sequence[1]);
		selected = selected_pair < 0 ? 0xFFU : (uint8_t)selected_pair;
	}

	if (selected == 0xFFU) {
		return false;
	}
	if (selected == 0U) {
		memcpy(__int_reg, slot0_int_regs, sizeof(slot0_int_regs));
		memcpy(__float_reg, slot0_float_regs, sizeof(slot0_float_regs));
	} else {
		preference_read_payload(preference_slot_writer(*pr, selected));
	}
	pr->active_slot = selected;
	pr->active_sequence = sequence[selected];
	pr->fw = preference_slot_writer(*pr, selected);
	return true;
}

void preference_writer_close(PreferenceWriter *pr) {
    pr->ready = false;
    flash_writer_close(&pr->fw);
}
