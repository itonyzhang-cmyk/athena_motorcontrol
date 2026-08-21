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
objdump_tool=${nm_tool%nm}objdump

if "$strings_tool" "$image" | grep -Eq 'BRINGUP_INJECT|SAFE_DIAGNOSTIC'; then
    echo "normal image verification failed: diagnostic banner linked" >&2
    exit 1
fi

for required in drv_init_config drv_enable_gd drv_service_enable \
    drv_enable_ready drv_disable_gd run_fsm \
    unpack_cmd torque_control commutate motor_gate_check safety_outputs_off \
    config_metadata_valid config_apply_defaults normal_can_frame_matches \
    normal_can_special_command; do
    if ! printf '%s\n' "$symbols" | awk '{print $3}' | grep -qx "$required"; then
        echo "normal image verification failed: missing symbol $required" >&2
        exit 1
    fi
done

if "$objdump_tool" -d --disassemble=drv_enable_gd "$image" | grep -q 'delay_1ms'; then
    echo "normal image verification failed: drv_enable_gd blocks on SysTick" >&2
    exit 1
fi

for forbidden in inject_handle_can inject_service inject_timer_tick; do
    if printf '%s\n' "$symbols" | awk '{print $3}' | grep -qx "$forbidden"; then
        echo "normal image verification failed: inject symbol linked: $forbidden" >&2
        exit 1
    fi
done

echo "normal image verification: PASS (normal application symbols and no inject path)"
