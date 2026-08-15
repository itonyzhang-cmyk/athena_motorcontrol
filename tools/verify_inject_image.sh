#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <arm-none-eabi-nm> <firmware.elf>" >&2
    exit 2
fi

nm_tool=$1
image=$2
symbols=$($nm_tool -g --defined-only "$image")
strings_tool=${nm_tool%nm}strings

if ! "$strings_tool" "$image" | grep -q "BRINGUP_INJECT"; then
    echo "inject image verification failed: BRINGUP_INJECT banner missing" >&2
    exit 1
fi

for forbidden in \
    fmc_unlock fmc_bank0_unlock fmc_bank1_unlock fmc_page_erase \
    fmc_mass_erase fmc_bank0_erase fmc_bank1_erase fmc_word_program \
    fmc_halfword_program fmc_word_reprogram ob_unlock ob_erase \
    ob_write_protection_enable ob_security_protection_config ob_user_write \
    ob_data_program flash_writer_open flash_writer_write_int \
    flash_writer_write_float preference_writer_open preference_writer_flush \
    preference_writer_write_int preference_writer_write_float \
    preference_writer_load drv_init_config drv_enable_gd drv_disable_gd \
    drv_calibrate commutate torque_control order_phases calibrate_encoder \
    update_fsm unpack_cmd pack_reply run_fsm set_dtc reset_foc zero_current
do
    if printf '%s\n' "$symbols" | awk '{print $3}' | grep -qx "$forbidden"; then
        echo "inject image verification failed: linked forbidden symbol $forbidden" >&2
        exit 1
    fi
done

require_symbol()
{
    if ! printf '%s\n' "$symbols" | awk '{print $3}' | grep -qx "$1"; then
        echo "inject image verification failed: missing required symbol $1" >&2
        exit 1
    fi
}

for required in \
    safety_force_outputs_off diagnostics_handle_can inject_init \
    inject_timer_tick inject_handle_can inject_snapshot inject_uart_report \
    analog_sample drv_spi_write drv_write_DCR drv_write_CSACR \
    drv_write_OCPCR drv_read_register drv_read_FSR1 drv_read_FSR2 \
    __flash_image_end__ __config_start__ __config_end__
do
    require_symbol "$required"
done

address_of()
{
    printf '%s\n' "$symbols" | awk -v name="$1" '$3 == name {print $1; exit}'
}

flash_end=$(address_of __flash_image_end__)
config_start=$(address_of __config_start__)
config_end=$(address_of __config_end__)

if [ "$config_start" != "0803c000" ] || [ "$config_end" != "0803d000" ]; then
    echo "inject image verification failed: unexpected config reservation $config_start..$config_end" >&2
    exit 1
fi

if [ $((16#$flash_end)) -ge $((16#$config_start)) ]; then
    echo "inject image verification failed: image overlaps reserved config pages" >&2
    exit 1
fi

echo "inject image verification: PASS (image end 0x$flash_end, config 0x$config_start..0x$config_end)"
