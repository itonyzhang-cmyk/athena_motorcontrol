#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <arm-none-eabi-nm> <firmware.elf>" >&2
    exit 2
fi

nm_tool=$1
image=$2
objdump_tool=${nm_tool%nm}objdump
symbols=$($nm_tool -g --defined-only "$image")

require_symbol()
{
    if ! printf '%s\n' "$symbols" | awk '{print $3}' | grep -qx "$1"; then
        echo "FWDGT self-test verification failed: missing symbol $1" >&2
        exit 1
    fi
}

require_symbol main
require_symbol runtime_watchdog_init
require_symbol fwdgt_config

main_disassembly=$($objdump_tool -d --disassemble=main "$image")
for required in MX_RCU_Init MX_GPIO_Init runtime_watchdog_init; do
    if ! printf '%s\n' "$main_disassembly" | grep -q "<$required>"; then
        echo "FWDGT self-test verification failed: main does not call $required" >&2
        exit 1
    fi
done

for forbidden in \
    MX_USART1_Init MX_TIM0_Init MX_CAN0_Init MX_SPI1_Init MX_SPI2_Init \
    MX_ADC01_Init MX_ADC2_Init MX_EXTI_Init inject_init drv_init_config \
    can_rx_init can_tx_init run_fsm; do
    if printf '%s\n' "$main_disassembly" | grep -q "<$forbidden>"; then
        echo "FWDGT self-test verification failed: main starts $forbidden" >&2
        exit 1
    fi
done

echo "FWDGT self-test verification: PASS (RCU/GPIO only before watchdog stall)"
