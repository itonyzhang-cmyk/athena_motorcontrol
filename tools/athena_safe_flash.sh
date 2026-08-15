#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_DIR="$(cd "${REPO_DIR}/.." && pwd)"

OPENOCD_BIN="${OPENOCD_BIN:-openocd}"
OPENOCD_INTERFACE="${OPENOCD_INTERFACE:-interface/stlink.cfg}"
OPENOCD_TARGET="${OPENOCD_TARGET:-target/stm32f1x.cfg}"
OPENOCD_SPEED_KHZ="${OPENOCD_SPEED_KHZ:-100}"
OPENOCD_CPUTAPID="${OPENOCD_CPUTAPID:-0x2ba01477}"

SAFE_IMAGE_DEFAULT="${WORKSPACE_DIR}/artifacts/athena_safe_diagnostic_ccf6522/motorcontrol.bin"
SAFE_IMAGE_SHA256="9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82"
SAFE_IMAGE_SIZE=29380
SAFE_IMAGE_BASE=0x08000000
SAFE_ERASE_SIZE=0x7800

FACTORY_IMAGE="${WORKSPACE_DIR}/backups/gd32f303ret6_factory_20260812_170113_CST/factory_flash_0x08000000_512KiB.bin"
FACTORY_IMAGE_SHA256="302f25ed7848ec22c77dbce177c79f548b6de502be71f06976034f8df9cb1ec7"
FLASH_SIZE=524288
FLASH_BASE=0x08000000
CONFIG_BASE=0x0803C000
CONFIG_SIZE=4096
OPTION_BASE=0x1FFFF800
OPTION_SIZE=16
EXPECTED_OPTION_SHA256="c0b942fbb9fe967ec0e7b675e080d48c930fc5fe3fde70f6dd6f9646fdffc0d3"
EXPECTED_DEBUG_WORD="17010414"

ACTION=""
OUTPUT_DIR=""
IMAGE="${SAFE_IMAGE_DEFAULT}"
WRITE_CONFIRMATION=""
WRITE_ACK=0
KEEP_TEMP=0
TEMP_DIR=""

usage() {
    cat <<'EOF'
Usage:
  tools/athena_safe_flash.sh preflight
  tools/athena_safe_flash.sh self-test
  tools/athena_safe_flash.sh backup [--output DIR]
  tools/athena_safe_flash.sh flash-safe [--image FILE] \
      --confirm-safe-sha 9824e058... --i-understand-this-writes-main-flash
  tools/athena_safe_flash.sh boot-safe [--image FILE]
  tools/athena_safe_flash.sh restore-factory \
      --confirm-factory-sha 302f25ed... --i-understand-this-writes-main-flash

Safety properties:
  * This tool never writes Option Bytes and never issues mass-erase/unprotect.
  * flash-safe accepts exactly the reviewed 29,380-byte SAFE_DIAGNOSTIC image.
  * flash-safe leaves the CPU halted after verified programming; boot-safe is a
    separate action.
  * restore-factory accepts only the recorded 512 KiB factory backup and also
    leaves the CPU halted.

Environment overrides:
  OPENOCD_BIN, OPENOCD_INTERFACE, OPENOCD_TARGET, OPENOCD_SPEED_KHZ,
  OPENOCD_CPUTAPID
EOF
}

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

cleanup() {
    if [[ -n "${TEMP_DIR}" && -d "${TEMP_DIR}" && ${KEEP_TEMP} -eq 0 ]]; then
        rm -rf -- "${TEMP_DIR}"
    fi
}
trap cleanup EXIT

sha256_file() {
    shasum -a 256 "$1" | awk '{print $1}'
}

file_size() {
    stat -f '%z' "$1"
}

check_safe_image() {
    [[ -f "${IMAGE}" ]] || fail "safe image not found: ${IMAGE}"
    [[ "$(file_size "${IMAGE}")" == "${SAFE_IMAGE_SIZE}" ]] ||
        fail "safe image size is not ${SAFE_IMAGE_SIZE} bytes"
    [[ "$(sha256_file "${IMAGE}")" == "${SAFE_IMAGE_SHA256}" ]] ||
        fail "safe image SHA-256 does not match the reviewed release"
}

check_factory_image() {
    [[ -f "${FACTORY_IMAGE}" ]] || fail "factory image not found: ${FACTORY_IMAGE}"
    [[ "$(file_size "${FACTORY_IMAGE}")" == "${FLASH_SIZE}" ]] ||
        fail "factory image is not exactly 512 KiB"
    [[ "$(sha256_file "${FACTORY_IMAGE}")" == "${FACTORY_IMAGE_SHA256}" ]] ||
        fail "factory image SHA-256 mismatch"
}

check_path_for_tcl() {
    [[ "$1" != *'{'* && "$1" != *'}'* && "$1" != *$'\n'* ]] ||
        fail "unsupported character in OpenOCD file path: $1"
}

openocd_capture() {
    local log_file="$1"
    local commands="$2"
    check_path_for_tcl "${log_file}"
    "${OPENOCD_BIN}" \
        -f "${OPENOCD_INTERFACE}" \
        -c "set CPUTAPID ${OPENOCD_CPUTAPID}" \
        -f "${OPENOCD_TARGET}" \
        -c "adapter speed ${OPENOCD_SPEED_KHZ}; ${commands}" \
        >"${log_file}" 2>&1 || {
            sed -n '1,240p' "${log_file}" >&2
            fail "OpenOCD command failed; target may remain halted"
        }
}

probe_and_read_options() {
    local directory="$1"
    local suffix="$2"
    local option_file="${directory}/option_bytes_${suffix}.bin"
    local log_file="${directory}/openocd_${suffix}.log"
    local commands target_voltage

    check_path_for_tcl "${option_file}"
    commands="init; reset halt; echo [format {DBG_WORD=0x%08X} [mrw 0xE0042000]]; echo [format {FLASH_SIZE=0x%04X} [mrh 0x1FFFF7E0]]; echo [format {OBSTAT=0x%08X} [mrw 0x4002201C]]; echo [format {WP=0x%08X} [mrw 0x40022020]]; dump_image {${option_file}} ${OPTION_BASE} ${OPTION_SIZE}; reset run; shutdown"
    openocd_capture "${log_file}" "${commands}"

    target_voltage="$(sed -n 's/.*Target voltage:[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "${log_file}" | head -1)"
    [[ -n "${target_voltage}" ]] || fail "OpenOCD did not report target voltage"
    awk -v voltage="${target_voltage}" 'BEGIN { exit !(voltage >= 3.0 && voltage <= 3.4) }' ||
        fail "target voltage ${target_voltage} V is outside the accepted 3.0..3.4 V range"
    grep -Eiq "DBG_WORD=0x${EXPECTED_DEBUG_WORD}" "${log_file}" ||
        fail "unexpected debug/device word; see ${log_file}"
    grep -Eiq "FLASH_SIZE=0x0200" "${log_file}" ||
        fail "target does not report 512 KiB Flash; see ${log_file}"
    [[ "$(file_size "${option_file}")" == "${OPTION_SIZE}" ]] ||
        fail "Option Bytes read returned the wrong length"
    [[ "$(sha256_file "${option_file}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes differ from the verified unprotected baseline"
}

make_temp_dir() {
    [[ -z "${TEMP_DIR}" ]] || return
    TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/athena-flash.XXXXXX")"
}

preflight() {
    make_temp_dir
    probe_and_read_options "${TEMP_DIR}" preflight
    printf 'PASS: ST-LINK target, 512 KiB Flash, and Option Bytes match the recorded baseline.\n'
}

self_test() {
    local recorded_options
    check_safe_image
    check_factory_image
    recorded_options="${WORKSPACE_DIR}/backups/gd32f303ret6_factory_20260812_170113_CST/option_bytes_0x1FFFF800_16B.bin"
    [[ -f "${recorded_options}" ]] || fail "recorded Option Bytes backup is missing"
    [[ "$(file_size "${recorded_options}")" == "${OPTION_SIZE}" ]] ||
        fail "recorded Option Bytes backup has the wrong length"
    [[ "$(sha256_file "${recorded_options}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "recorded Option Bytes hash mismatch"
    (( SAFE_IMAGE_SIZE <= SAFE_ERASE_SIZE )) || fail "safe image exceeds its erase range"
    (( SAFE_IMAGE_BASE + SAFE_ERASE_SIZE <= CONFIG_BASE )) ||
        fail "safe erase range overlaps reserved configuration pages"
    printf 'PASS: reviewed safe/factory/Option Bytes artifacts and non-overlapping safe erase range.\n'
}

backup_current() {
    local timestamp first second option log1 log2
    timestamp="$(date '+%Y%m%d_%H%M%S_%Z')"
    if [[ -z "${OUTPUT_DIR}" ]]; then
        OUTPUT_DIR="${WORKSPACE_DIR}/backups/gd32f303ret6_preflash_${timestamp}"
    fi
    [[ ! -e "${OUTPUT_DIR}" ]] || fail "backup output already exists: ${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"
    KEEP_TEMP=1
    TEMP_DIR="${OUTPUT_DIR}"

    probe_and_read_options "${OUTPUT_DIR}" preflight
    first="${OUTPUT_DIR}/flash_read_1_512KiB.bin"
    second="${OUTPUT_DIR}/flash_read_2_512KiB.bin"
    log1="${OUTPUT_DIR}/openocd_flash_read_1.log"
    log2="${OUTPUT_DIR}/openocd_flash_read_2.log"
    check_path_for_tcl "${first}"
    check_path_for_tcl "${second}"

    openocd_capture "${log1}" \
        "init; reset halt; dump_image {${first}} ${FLASH_BASE} ${FLASH_SIZE}; reset run; shutdown"
    openocd_capture "${log2}" \
        "init; reset halt; dump_image {${second}} ${FLASH_BASE} ${FLASH_SIZE}; reset run; shutdown"

    [[ "$(file_size "${first}")" == "${FLASH_SIZE}" ]] || fail "first Flash read is incomplete"
    [[ "$(file_size "${second}")" == "${FLASH_SIZE}" ]] || fail "second Flash read is incomplete"
    cmp -s "${first}" "${second}" || fail "the two current-state Flash reads differ"

    option="${OUTPUT_DIR}/option_bytes_preflight.bin"
    {
        printf '# GD32F303RET6 pre-flash current-state backup\n\n'
        printf -- '- Created: %s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
        printf -- '- Operation: SWD read-only; no erase/program/unprotect/Option Bytes write\n'
        printf -- '- Flash: two independent 512 KiB reads, byte-identical\n'
        printf -- '- Option Bytes: 16-byte read, matched verified baseline\n'
        printf -- '- OpenOCD interface: `%s`\n' "${OPENOCD_INTERFACE}"
        printf -- '- OpenOCD target: `%s`\n' "${OPENOCD_TARGET}"
        printf -- '- Adapter speed: %s kHz\n' "${OPENOCD_SPEED_KHZ}"
    } >"${OUTPUT_DIR}/README.md"
    (
        cd "${OUTPUT_DIR}"
        shasum -a 256 "$(basename "${first}")" "$(basename "${second}")" \
            "$(basename "${option}")" >SHA256SUMS
    )
    printf 'PASS: current-state backup created at %s\n' "${OUTPUT_DIR}"
}

dump_config() {
    local output="$1"
    local log="$2"
    check_path_for_tcl "${output}"
    openocd_capture "${log}" \
        "init; reset halt; dump_image {${output}} ${CONFIG_BASE} ${CONFIG_SIZE}; shutdown"
}

flash_safe() {
    local backup_dir config_before config_after readback option_after log
    check_safe_image
    [[ ${WRITE_ACK} -eq 1 ]] || fail "missing --i-understand-this-writes-main-flash"
    [[ "${WRITE_CONFIRMATION}" == "${SAFE_IMAGE_SHA256}" ]] ||
        fail "missing exact --confirm-safe-sha value"

    backup_current
    backup_dir="${OUTPUT_DIR}"
    config_before="${backup_dir}/config_before_0x0803C000_4KiB.bin"
    config_after="${backup_dir}/config_after_0x0803C000_4KiB.bin"
    readback="${backup_dir}/safe_image_readback_29380B.bin"
    option_after="${backup_dir}/option_bytes_after_safe_flash.bin"

    dump_config "${config_before}" "${backup_dir}/openocd_config_before.log"
    check_path_for_tcl "${IMAGE}"
    log="${backup_dir}/openocd_flash_safe.log"
    openocd_capture "${log}" \
        "init; reset halt; flash erase_address ${SAFE_IMAGE_BASE} ${SAFE_ERASE_SIZE}; flash write_image {${IMAGE}} ${SAFE_IMAGE_BASE} bin; verify_image {${IMAGE}} ${SAFE_IMAGE_BASE} bin; shutdown"

    check_path_for_tcl "${readback}"
    check_path_for_tcl "${option_after}"
    openocd_capture "${backup_dir}/openocd_postflash_readback.log" \
        "init; reset halt; dump_image {${readback}} ${SAFE_IMAGE_BASE} ${SAFE_IMAGE_SIZE}; dump_image {${option_after}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    dump_config "${config_after}" "${backup_dir}/openocd_config_after.log"

    cmp -s "${IMAGE}" "${readback}" || fail "programmed image readback differs; CPU remains halted"
    cmp -s "${config_before}" "${config_after}" || fail "reserved configuration range changed; CPU remains halted"
    [[ "$(sha256_file "${option_after}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes changed unexpectedly; CPU remains halted"

    shasum -a 256 "${config_before}" "${config_after}" "${readback}" \
        "${option_after}" >>"${backup_dir}/SHA256SUMS"
    printf 'PASS: SAFE_DIAGNOSTIC programmed and read back exactly.\n'
    printf 'Programmed image range: %s..0x%08X; config page is unchanged.\n' \
        "${SAFE_IMAGE_BASE}" "$((SAFE_IMAGE_BASE + SAFE_IMAGE_SIZE - 1))"
    printf 'The CPU remains halted. Run boot-safe only after the physical bench is ready.\n'
}

boot_safe() {
    local readback option_file
    check_safe_image
    make_temp_dir
    readback="${TEMP_DIR}/safe_image_before_boot.bin"
    option_file="${TEMP_DIR}/option_bytes_before_boot.bin"
    check_path_for_tcl "${readback}"
    check_path_for_tcl "${option_file}"
    openocd_capture "${TEMP_DIR}/openocd_boot_safe.log" \
        "init; reset halt; dump_image {${readback}} ${SAFE_IMAGE_BASE} ${SAFE_IMAGE_SIZE}; dump_image {${option_file}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    cmp -s "${IMAGE}" "${readback}" || fail "target does not contain the reviewed SAFE_DIAGNOSTIC image"
    [[ "$(sha256_file "${option_file}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes differ; refusing to boot"
    openocd_capture "${TEMP_DIR}/openocd_reset_run.log" "init; reset run; shutdown"
    printf 'PASS: reviewed SAFE_DIAGNOSTIC image reset and started.\n'
}

restore_factory() {
    local timestamp output readback option_after
    check_factory_image
    [[ ${WRITE_ACK} -eq 1 ]] || fail "missing --i-understand-this-writes-main-flash"
    [[ "${WRITE_CONFIRMATION}" == "${FACTORY_IMAGE_SHA256}" ]] ||
        fail "missing exact --confirm-factory-sha value"
    preflight
    cleanup
    TEMP_DIR=""
    timestamp="$(date '+%Y%m%d_%H%M%S_%Z')"
    output="${WORKSPACE_DIR}/backups/factory_restore_verification_${timestamp}"
    mkdir -p "${output}"
    readback="${output}/factory_restore_readback_512KiB.bin"
    option_after="${output}/option_bytes_after_factory_restore.bin"
    check_path_for_tcl "${FACTORY_IMAGE}"
    check_path_for_tcl "${readback}"
    check_path_for_tcl "${option_after}"

    openocd_capture "${output}/openocd_restore_factory.log" \
        "init; reset halt; flash erase_address ${FLASH_BASE} ${FLASH_SIZE}; flash write_image {${FACTORY_IMAGE}} ${FLASH_BASE} bin; verify_image {${FACTORY_IMAGE}} ${FLASH_BASE} bin; shutdown"
    openocd_capture "${output}/openocd_restore_readback.log" \
        "init; reset halt; dump_image {${readback}} ${FLASH_BASE} ${FLASH_SIZE}; dump_image {${option_after}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    cmp -s "${FACTORY_IMAGE}" "${readback}" || fail "factory restore readback differs; CPU remains halted"
    [[ "$(sha256_file "${option_after}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes changed unexpectedly; CPU remains halted"
    shasum -a 256 "${readback}" "${option_after}" >"${output}/SHA256SUMS"
    printf 'PASS: factory main Flash restored and verified. Option Bytes were not written.\n'
    printf 'The CPU remains halted; booting factory firmware requires a separate reviewed action.\n'
}

[[ $# -gt 0 ]] || { usage; exit 2; }
ACTION="$1"
shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        --output) [[ $# -ge 2 ]] || fail "--output requires a directory"; OUTPUT_DIR="$2"; shift 2 ;;
        --image) [[ $# -ge 2 ]] || fail "--image requires a file"; IMAGE="$2"; shift 2 ;;
        --confirm-safe-sha) [[ $# -ge 2 ]] || fail "--confirm-safe-sha requires a value"; WRITE_CONFIRMATION="$2"; shift 2 ;;
        --confirm-factory-sha) [[ $# -ge 2 ]] || fail "--confirm-factory-sha requires a value"; WRITE_CONFIRMATION="$2"; shift 2 ;;
        --i-understand-this-writes-main-flash) WRITE_ACK=1; shift ;;
        --keep-temp) KEEP_TEMP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

case "${ACTION}" in
    self-test) self_test ;;
    preflight) preflight ;;
    backup) backup_current ;;
    flash-safe) flash_safe ;;
    boot-safe) boot_safe ;;
    restore-factory) restore_factory ;;
    -h|--help|help) usage ;;
    *) usage; fail "unknown action: ${ACTION}" ;;
esac
