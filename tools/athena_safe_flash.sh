#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
WORKSPACE_DIR="$(cd "${REPO_DIR}/.." && pwd)"

if [[ -z "${OPENOCD_BIN:-}" ]]; then
    if [[ -x /opt/homebrew/bin/openocd ]]; then
        OPENOCD_BIN=/opt/homebrew/bin/openocd
    elif [[ -x /usr/local/bin/openocd ]]; then
        OPENOCD_BIN=/usr/local/bin/openocd
    else
        OPENOCD_BIN=openocd
    fi
fi
OPENOCD_INTERFACE="${OPENOCD_INTERFACE:-interface/stlink.cfg}"
OPENOCD_TARGET="${OPENOCD_TARGET:-target/stm32f1x.cfg}"
OPENOCD_SCRIPT="${OPENOCD_SCRIPT:-}"
OPENOCD_SPEED_KHZ="${OPENOCD_SPEED_KHZ:-100}"
OPENOCD_CPUTAPID="${OPENOCD_CPUTAPID:-0x2ba01477}"

SAFE_IMAGE_DEFAULT="${WORKSPACE_DIR}/artifacts/athena_safe_diagnostic_factory_can_96f03a7a/motorcontrol.bin"
SAFE_IMAGE_SHA256="96f03a7a64a6f450734f5bc851731691d556f9e7869243d7ee35420eb34aee52"
SAFE_IMAGE_SIZE=29380
SAFE_IMAGE_BASE=0x08000000
SAFE_ERASE_SIZE=0x7800

INJECT_IMAGE_DEFAULT="${REPO_DIR}/tmp/validation/artifacts/athena_inject_poen_stage_20260820/motorcontrol.bin"
INJECT_IMAGE_SHA256="55e8d6816e087d8308888960a5ba91545a8c5ab521ca3b625bff137268bb8714"
INJECT_IMAGE_SIZE=33764
INJECT_IMAGE_BASE=0x08000000
INJECT_ERASE_SIZE=0x8800
NORMAL_IMAGE_DEFAULT="${REPO_DIR}/artifacts/athena_normal_runtime_diagnostics_20260824/motorcontrol.bin"
NORMAL_IMAGE_SHA256="813463d0fe59e2fb17d731ca8708d2cec4450a8100a1068a146c3286ce2f49a1"
NORMAL_IMAGE_SIZE=55836
NORMAL_IMAGE_BASE=0x08000000
# The CAN RAM debug normal image is currently just under 0xE804 bytes. Keep a
# page-aligned 0xF000 window (60 KiB, 2048-byte pages), still far below the reserved
# CONFIG base 0x0803C000.
# Normal application may grow up to the reserved-config boundary.  Keep the
# erase range page aligned and never erase CONFIG_BASE or the A/B preferences.
NORMAL_ERASE_SIZE=0x3C000

# Keep the audited normal image selectable without restarting the WebUI. The
# same JSON is read by athena_bench_webui.py; these defaults remain a safe
# fallback if the config file is temporarily unavailable.
# A one-shot override permits a hash-locked experimental image without
# changing the WebUI's selected normal-release artifact.
NORMAL_CONFIG="${NORMAL_CONFIG:-${REPO_DIR}/athena_bench_webui.json}"
if [[ -f "${NORMAL_CONFIG}" ]] && command -v python3 >/dev/null 2>&1; then
    _normal_config_line="$(python3 - "${NORMAL_CONFIG}" <<'PY'
import json, sys
from pathlib import Path
try:
    data = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    print(f'{data["normal_image"]}\t{data["normal_sha"]}')
except Exception:
    pass
PY
)"
    IFS=$'\t' read -r _normal_image_rel _normal_image_sha <<< "${_normal_config_line}"
    if [[ -n "${_normal_image_rel}" && -n "${_normal_image_sha}" ]]; then
        NORMAL_IMAGE_DEFAULT="${REPO_DIR}/${_normal_image_rel}"
        NORMAL_IMAGE_SHA256="${_normal_image_sha}"
        NORMAL_IMAGE_SIZE="$(wc -c < "${NORMAL_IMAGE_DEFAULT}" | tr -d ' ')"
    fi
fi
EVAL_IMAGE_DEFAULT="${REPO_DIR}/tmp/validation/artifacts/athena_normal_eval_20260821/motorcontrol.bin"
EVAL_IMAGE_SHA256="538da9dc8aac7c4e144de6d6b8f126fbc8f7fee348ef1976ed6b9a6e72330afa"
EVAL_IMAGE_SIZE=52372
EVAL_IMAGE_BASE=0x08000000
EVAL_ERASE_SIZE=0xD000
FLASH_PAGE_SIZE=2048

FACTORY_IMAGE="${WORKSPACE_DIR}/backups/gd32f303ret6_factory_20260812_170113_CST/factory_flash_0x08000000_512KiB.bin"
FACTORY_IMAGE_SHA256="302f25ed7848ec22c77dbce177c79f548b6de502be71f06976034f8df9cb1ec7"
FLASH_SIZE=524288
FLASH_BASE=0x08000000
CONFIG_BASE=0x0803C000
CONFIG_SIZE=4096
OPTION_BASE=0x1FFFF800
OPTION_SIZE=16
UID0_ADDR=0x1FFFF7E8
UID1_ADDR=0x1FFFF7EC
UID2_ADDR=0x1FFFF7F0
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
  tools/athena_safe_flash.sh identify
  tools/athena_safe_flash.sh preflight
  tools/athena_safe_flash.sh self-test
  tools/athena_safe_flash.sh backup [--output DIR]
  tools/athena_safe_flash.sh flash-safe [--image FILE] \
      --confirm-safe-sha 51adc69a... --i-understand-this-writes-main-flash
  tools/athena_safe_flash.sh boot-safe [--image FILE]
  tools/athena_safe_flash.sh flash-inject \
      --confirm-inject-sha 9bde97ea... --i-understand-this-writes-main-flash
  tools/athena_safe_flash.sh boot-inject
  tools/athena_safe_flash.sh flash-normal \
      --confirm-normal-sha bac3554f... --i-understand-this-writes-main-flash
  tools/athena_safe_flash.sh boot-normal
  tools/athena_safe_flash.sh flash-normal-eval \
      --confirm-eval-sha 538da9dc... --i-understand-this-writes-main-flash
  tools/athena_safe_flash.sh boot-normal-eval
  tools/athena_safe_flash.sh restore-factory \
      --confirm-factory-sha 302f25ed... --i-understand-this-writes-main-flash

Safety properties:
  * This tool never writes Option Bytes and never issues mass-erase/unprotect.
  * flash-safe accepts exactly the reviewed 29,380-byte SAFE_DIAGNOSTIC image.
  * flash-inject accepts exactly the reviewed 33,764-byte BRINGUP_INJECT image.
  * flash-normal accepts exactly the audited normal application image and is a
    separate action from the diagnostic profiles.
  * flash-inject erases, writes, and verifies 2 KiB pages individually.
  * flash-safe leaves the CPU halted after verified programming; boot-safe is a
    separate action.
  * restore-factory accepts only the recorded 512 KiB factory backup and also
    leaves the CPU halted.

Environment overrides:
  OPENOCD_BIN, OPENOCD_INTERFACE, OPENOCD_TARGET, OPENOCD_SPEED_KHZ,
  OPENOCD_CPUTAPID, NORMAL_CONFIG
EOF
}

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

progress() {
    printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"
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

check_inject_image() {
    [[ -f "${INJECT_IMAGE_DEFAULT}" ]] ||
        fail "inject image not found: ${INJECT_IMAGE_DEFAULT}"
    [[ "$(file_size "${INJECT_IMAGE_DEFAULT}")" == "${INJECT_IMAGE_SIZE}" ]] ||
        fail "inject image size is not ${INJECT_IMAGE_SIZE} bytes"
    [[ "$(sha256_file "${INJECT_IMAGE_DEFAULT}")" == "${INJECT_IMAGE_SHA256}" ]] ||
        fail "inject image SHA-256 does not match the reviewed release"
}

check_normal_image() {
    [[ -f "${NORMAL_IMAGE_DEFAULT}" ]] ||
        fail "normal image not found: ${NORMAL_IMAGE_DEFAULT}"
    [[ "$(file_size "${NORMAL_IMAGE_DEFAULT}")" == "${NORMAL_IMAGE_SIZE}" ]] ||
        fail "normal image size is not ${NORMAL_IMAGE_SIZE} bytes"
    [[ "$(sha256_file "${NORMAL_IMAGE_DEFAULT}")" == "${NORMAL_IMAGE_SHA256}" ]] ||
        fail "normal image SHA-256 does not match the audited release"
}

check_eval_image() {
    [[ -f "${EVAL_IMAGE_DEFAULT}" ]] || fail "evaluation image not found: ${EVAL_IMAGE_DEFAULT}"
    [[ "$(file_size "${EVAL_IMAGE_DEFAULT}")" == "${EVAL_IMAGE_SIZE}" ]] ||
        fail "evaluation image size is not ${EVAL_IMAGE_SIZE} bytes"
    [[ "$(sha256_file "${EVAL_IMAGE_DEFAULT}")" == "${EVAL_IMAGE_SHA256}" ]] ||
        fail "evaluation image SHA-256 does not match the known-good CAN candidate"
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
    local -a openocd_args
    check_path_for_tcl "${log_file}"
    openocd_args=("${OPENOCD_BIN}")
    if [[ -n "${OPENOCD_SCRIPT}" ]]; then
        openocd_args+=( -s "${OPENOCD_SCRIPT}" )
    fi
    "${openocd_args[@]}" \
        -f "${OPENOCD_INTERFACE}" \
        -c "set CPUTAPID ${OPENOCD_CPUTAPID}" \
        -f "${OPENOCD_TARGET}" \
        -c "adapter speed ${OPENOCD_SPEED_KHZ}; ${commands}" \
        >"${log_file}" 2>&1 || {
            sed -n '1,240p' "${log_file}" >&2
            fail "OpenOCD command failed; target may remain halted"
        }
}

flash_image_by_page() {
    local image="$1"
    local image_size="$2"
    local base="$3"
    local erase_size="$4"
    local output_dir="$5"
    local label="$6"
    local page_count page_index offset address remaining chunk_size page_file log_file

    (( erase_size % FLASH_PAGE_SIZE == 0 )) ||
        fail "${label} erase range is not page aligned"
    (( image_size <= erase_size )) || fail "${label} image exceeds erase range"
    page_count=$((erase_size / FLASH_PAGE_SIZE))
    printf 'Programming %s in %d page(s) of %d bytes.\n' \
        "${label}" "${page_count}" "${FLASH_PAGE_SIZE}"

    for ((page_index = 0; page_index < page_count; ++page_index)); do
        offset=$((page_index * FLASH_PAGE_SIZE))
        address=$((base + offset))
        remaining=$((image_size - offset))
        log_file="${output_dir}/openocd_${label}_page_$(printf '%02d' "${page_index}").log"

        if ((remaining > 0)); then
            chunk_size=${remaining}
            ((chunk_size > FLASH_PAGE_SIZE)) && chunk_size=${FLASH_PAGE_SIZE}
            page_file="${TEMP_DIR}/${label}_page_$(printf '%02d' "${page_index}").bin"
            dd if="${image}" of="${page_file}" bs=${FLASH_PAGE_SIZE} \
                skip=${page_index} count=1 status=none
            [[ "$(file_size "${page_file}")" == "${chunk_size}" ]] ||
                fail "${label} page ${page_index} extraction has an unexpected size"
            check_path_for_tcl "${page_file}"
            printf '[%02d/%02d] erase/write/verify 0x%08X (%d bytes)\n' \
                "$((page_index + 1))" "${page_count}" "${address}" "${chunk_size}"
            openocd_capture "${log_file}" \
                "init; reset halt; flash erase_address 0x$(printf '%08X' "${address}") ${FLASH_PAGE_SIZE}; flash write_image {${page_file}} 0x$(printf '%08X' "${address}") bin; verify_image {${page_file}} 0x$(printf '%08X' "${address}") bin; shutdown"
            printf '[%02d/%02d] verified 0x%08X\n' \
                "$((page_index + 1))" "${page_count}" "${address}"
        else
            printf '[%02d/%02d] erase only 0x%08X (%d bytes)\n' \
                "$((page_index + 1))" "${page_count}" "${address}" "${FLASH_PAGE_SIZE}"
            openocd_capture "${log_file}" \
                "init; reset halt; flash erase_address 0x$(printf '%08X' "${address}") ${FLASH_PAGE_SIZE}; shutdown"
            printf '[%02d/%02d] erased 0x%08X\n' \
                "$((page_index + 1))" "${page_count}" "${address}"
        fi
    done
}

probe_and_read_options() {
    local directory="$1"
    local suffix="$2"
    local option_file="${directory}/option_bytes_${suffix}.bin"
    local log_file="${directory}/openocd_${suffix}.log"
    local commands target_voltage

    progress "preflight: connecting to ST-LINK and reading target identity/Option Bytes."
    check_path_for_tcl "${option_file}"
    commands="init; reset halt; echo [format {DBG_WORD=0x%08X} [mrw 0xE0042000]]; echo [format {FLASH_SIZE=0x%04X} [mrh 0x1FFFF7E0]]; echo [format {UID0=0x%08X} [mrw ${UID0_ADDR}]]; echo [format {UID1=0x%08X} [mrw ${UID1_ADDR}]]; echo [format {UID2=0x%08X} [mrw ${UID2_ADDR}]]; echo [format {OBSTAT=0x%08X} [mrw 0x4002201C]]; echo [format {WP=0x%08X} [mrw 0x40022020]]; dump_image {${option_file}} ${OPTION_BASE} ${OPTION_SIZE}; reset run; shutdown"
    openocd_capture "${log_file}" "${commands}"

    target_voltage="$(sed -n 's/.*Target voltage:[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "${log_file}" | head -1)"
    [[ -n "${target_voltage}" ]] || fail "OpenOCD did not report target voltage"
    awk -v voltage="${target_voltage}" 'BEGIN { exit !(voltage >= 3.0 && voltage <= 3.4) }' ||
        fail "target voltage ${target_voltage} V is outside the accepted 3.0..3.4 V range"
    grep -Eiq "DBG_WORD=0x${EXPECTED_DEBUG_WORD}" "${log_file}" ||
        fail "unexpected debug/device word; see ${log_file}"
    grep -Eiq "FLASH_SIZE=0x0200" "${log_file}" ||
        fail "target does not report 512 KiB Flash; see ${log_file}"
    uid_from_log "${log_file}" >/dev/null
    [[ "$(file_size "${option_file}")" == "${OPTION_SIZE}" ]] ||
        fail "Option Bytes read returned the wrong length"
    [[ "$(sha256_file "${option_file}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes differ from the verified unprotected baseline"
    progress "preflight: target identity and Option Bytes verified."
}

uid_from_log() {
    local log_file="$1"
    local uid0 uid1 uid2
    uid0="$(sed -n 's/.*UID0=0x\([[:xdigit:]]\{8\}\).*/\1/p' "${log_file}" | head -1)"
    uid1="$(sed -n 's/.*UID1=0x\([[:xdigit:]]\{8\}\).*/\1/p' "${log_file}" | head -1)"
    uid2="$(sed -n 's/.*UID2=0x\([[:xdigit:]]\{8\}\).*/\1/p' "${log_file}" | head -1)"
    [[ -n "${uid0}" && -n "${uid1}" && -n "${uid2}" ]] ||
        fail "target did not return a complete 96-bit UID; see ${log_file}"
    printf '%s-%s-%s\n' \
        "$(printf '%s' "${uid2}" | tr '[:lower:]' '[:upper:]')" \
        "$(printf '%s' "${uid1}" | tr '[:lower:]' '[:upper:]')" \
        "$(printf '%s' "${uid0}" | tr '[:lower:]' '[:upper:]')"
}

identify() {
    local log_file target_voltage uid
    make_temp_dir
    log_file="${TEMP_DIR}/openocd_identify.log"
    openocd_capture "${log_file}" \
        "init; reset halt; echo [format {DBG_WORD=0x%08X} [mrw 0xE0042000]]; echo [format {FLASH_SIZE=0x%04X} [mrh 0x1FFFF7E0]]; echo [format {UID0=0x%08X} [mrw ${UID0_ADDR}]]; echo [format {UID1=0x%08X} [mrw ${UID1_ADDR}]]; echo [format {UID2=0x%08X} [mrw ${UID2_ADDR}]]; reset run; shutdown"
    target_voltage="$(sed -n 's/.*Target voltage:[[:space:]]*\([0-9][0-9.]*\).*/\1/p' "${log_file}" | head -1)"
    [[ -n "${target_voltage}" ]] || fail "OpenOCD did not report target voltage"
    grep -Eiq "DBG_WORD=0x${EXPECTED_DEBUG_WORD}" "${log_file}" ||
        fail "unexpected debug/device word; see ${log_file}"
    grep -Eiq "FLASH_SIZE=0x0200" "${log_file}" ||
        fail "target does not report 512 KiB Flash; see ${log_file}"
    uid="$(uid_from_log "${log_file}")"
    printf 'UID=%s\nTARGET_VOLTAGE=%s V\n' "${uid}" "${target_voltage}"
}

make_temp_dir() {
    [[ -z "${TEMP_DIR}" ]] || return
    TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/athena-flash.XXXXXX")"
}

preflight() {
    local uid
    make_temp_dir
    probe_and_read_options "${TEMP_DIR}" preflight
    uid="$(uid_from_log "${TEMP_DIR}/openocd_preflight.log")"
    printf 'PASS: UID=%s; ST-LINK target, 512 KiB Flash, and Option Bytes match the recorded baseline.\n' "${uid}"
}

self_test() {
    local recorded_options
    check_safe_image
    check_inject_image
    check_normal_image
    check_eval_image
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
    (( INJECT_IMAGE_SIZE <= INJECT_ERASE_SIZE )) ||
        fail "inject image exceeds its erase range"
    (( INJECT_IMAGE_BASE + INJECT_ERASE_SIZE <= CONFIG_BASE )) ||
        fail "inject erase range overlaps reserved configuration pages"
    (( NORMAL_IMAGE_SIZE <= NORMAL_ERASE_SIZE )) ||
        fail "normal image exceeds its erase range"
  (( NORMAL_IMAGE_BASE + NORMAL_ERASE_SIZE <= CONFIG_BASE )) ||
        fail "normal erase range overlaps reserved configuration pages"
    (( EVAL_IMAGE_SIZE <= EVAL_ERASE_SIZE )) ||
        fail "evaluation image exceeds its erase range"
    (( EVAL_IMAGE_BASE + EVAL_ERASE_SIZE <= CONFIG_BASE )) ||
        fail "evaluation erase range overlaps reserved configuration pages"
    printf 'PASS: reviewed safe/inject/normal/evaluation/factory artifacts and non-overlapping erase ranges.\n'
}

backup_current() {
    local timestamp first second option log1 log2 uid
    timestamp="$(date '+%Y%m%d_%H%M%S_%Z')"
    if [[ -z "${OUTPUT_DIR}" ]]; then
        OUTPUT_DIR="${WORKSPACE_DIR}/backups/gd32f303ret6_preflash_${timestamp}"
    fi
    [[ ! -e "${OUTPUT_DIR}" ]] || fail "backup output already exists: ${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"
    KEEP_TEMP=1
    TEMP_DIR="${OUTPUT_DIR}"

    progress "backup: creating mandatory pre-write snapshot at ${OUTPUT_DIR}."
    probe_and_read_options "${OUTPUT_DIR}" preflight
    first="${OUTPUT_DIR}/flash_read_1_512KiB.bin"
    second="${OUTPUT_DIR}/flash_read_2_512KiB.bin"
    log1="${OUTPUT_DIR}/openocd_flash_read_1.log"
    log2="${OUTPUT_DIR}/openocd_flash_read_2.log"
    check_path_for_tcl "${first}"
    check_path_for_tcl "${second}"

    progress "backup: reading full main Flash (1/2, 512 KiB); no erase/program occurs."
    openocd_capture "${log1}" \
        "init; reset halt; dump_image {${first}} ${FLASH_BASE} ${FLASH_SIZE}; reset run; shutdown"
    progress "backup: full main Flash read 1/2 complete."
    progress "backup: reading full main Flash (2/2, 512 KiB); no erase/program occurs."
    openocd_capture "${log2}" \
        "init; reset halt; dump_image {${second}} ${FLASH_BASE} ${FLASH_SIZE}; reset run; shutdown"
    progress "backup: full main Flash read 2/2 complete; comparing both copies."

    [[ "$(file_size "${first}")" == "${FLASH_SIZE}" ]] || fail "first Flash read is incomplete"
    [[ "$(file_size "${second}")" == "${FLASH_SIZE}" ]] || fail "second Flash read is incomplete"
    cmp -s "${first}" "${second}" || fail "the two current-state Flash reads differ"

    option="${OUTPUT_DIR}/option_bytes_preflight.bin"
    uid="$(uid_from_log "${OUTPUT_DIR}/openocd_preflight.log")"
    {
        printf '# GD32F303RET6 pre-flash current-state backup\n\n'
        printf -- '- Created: %s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
        printf -- '- Operation: SWD read-only; no erase/program/unprotect/Option Bytes write\n'
        printf -- '- Flash: two independent 512 KiB reads, byte-identical\n'
        printf -- '- Option Bytes: 16-byte read, matched verified baseline\n'
        printf -- '- Device UID: `%s` (UID[95:64]..UID[31:0])\n' "${uid}"
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

flash_inject() {
    local backup_dir config_before config_after readback option_after
    check_inject_image
    [[ ${WRITE_ACK} -eq 1 ]] || fail "missing --i-understand-this-writes-main-flash"
    [[ "${WRITE_CONFIRMATION}" == "${INJECT_IMAGE_SHA256}" ]] ||
        fail "missing exact --confirm-inject-sha value"

    progress "flash-inject: reviewed ${INJECT_IMAGE_SIZE}-byte image accepted; main Flash has not been changed."
    progress "flash-inject: starting mandatory read-only backup before any erase/program."
    backup_current
    backup_dir="${OUTPUT_DIR}"
    config_before="${backup_dir}/config_before_0x0803C000_4KiB.bin"
    config_after="${backup_dir}/config_after_0x0803C000_4KiB.bin"
    readback="${backup_dir}/inject_image_readback_${INJECT_IMAGE_SIZE}B.bin"
    option_after="${backup_dir}/option_bytes_after_inject_flash.bin"

    progress "flash-inject: reading reserved configuration range before programming."
    dump_config "${config_before}" "${backup_dir}/openocd_config_before.log"
    progress "flash-inject: reserved configuration range recorded; page programming begins next."
    check_path_for_tcl "${INJECT_IMAGE_DEFAULT}"
    flash_image_by_page "${INJECT_IMAGE_DEFAULT}" "${INJECT_IMAGE_SIZE}" \
        "${INJECT_IMAGE_BASE}" "${INJECT_ERASE_SIZE}" "${backup_dir}" "flash_inject"

    check_path_for_tcl "${readback}"
    check_path_for_tcl "${option_after}"
    progress "flash-inject: all pages verified; reading complete image and Option Bytes for final verification."
    openocd_capture "${backup_dir}/openocd_postflash_readback.log" \
        "init; reset halt; dump_image {${readback}} ${INJECT_IMAGE_BASE} ${INJECT_IMAGE_SIZE}; dump_image {${option_after}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    progress "flash-inject: reading reserved configuration range after programming."
    dump_config "${config_after}" "${backup_dir}/openocd_config_after.log"

    cmp -s "${INJECT_IMAGE_DEFAULT}" "${readback}" ||
        fail "programmed inject image readback differs; CPU remains halted"
    cmp -s "${config_before}" "${config_after}" ||
        fail "reserved configuration range changed; CPU remains halted"
    [[ "$(sha256_file "${option_after}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes changed unexpectedly; CPU remains halted"

    shasum -a 256 "${config_before}" "${config_after}" "${readback}" \
        "${option_after}" >>"${backup_dir}/SHA256SUMS"
    printf 'PASS: BRINGUP_INJECT programmed and read back exactly.\n'
    printf 'Programmed image range: %s..0x%08X; config page is unchanged.\n' \
        "${INJECT_IMAGE_BASE}" "$((INJECT_IMAGE_BASE + INJECT_IMAGE_SIZE - 1))"
    printf 'The CPU remains halted. Run boot-inject only after the physical bench is ready.\n'
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

boot_inject() {
    local readback option_file
    check_inject_image
    make_temp_dir
    readback="${TEMP_DIR}/inject_image_before_boot.bin"
    option_file="${TEMP_DIR}/option_bytes_before_boot.bin"
    check_path_for_tcl "${readback}"
    check_path_for_tcl "${option_file}"
    openocd_capture "${TEMP_DIR}/openocd_boot_inject.log" \
        "init; reset halt; dump_image {${readback}} ${INJECT_IMAGE_BASE} ${INJECT_IMAGE_SIZE}; dump_image {${option_file}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    cmp -s "${INJECT_IMAGE_DEFAULT}" "${readback}" ||
        fail "target does not contain the reviewed BRINGUP_INJECT image"
    [[ "$(sha256_file "${option_file}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes differ; refusing to boot"
    openocd_capture "${TEMP_DIR}/openocd_reset_run.log" "init; reset run; shutdown"
    printf 'PASS: reviewed BRINGUP_INJECT image reset and started.\n'
}

flash_normal() {
    local backup_dir config_before config_after readback option_after
    check_normal_image
    [[ ${WRITE_ACK} -eq 1 ]] || fail "missing --i-understand-this-writes-main-flash"
    [[ "${WRITE_CONFIRMATION}" == "${NORMAL_IMAGE_SHA256}" ]] ||
        fail "missing exact --confirm-normal-sha value"

    progress "flash-normal: audited ${NORMAL_IMAGE_SIZE}-byte image accepted; main Flash has not been changed."
    backup_current
    backup_dir="${OUTPUT_DIR}"
    config_before="${backup_dir}/config_before_0x0803C000_4KiB.bin"
    config_after="${backup_dir}/config_after_0x0803C000_4KiB.bin"
    readback="${backup_dir}/normal_image_readback_${NORMAL_IMAGE_SIZE}B.bin"
    option_after="${backup_dir}/option_bytes_after_normal_flash.bin"

    dump_config "${config_before}" "${backup_dir}/openocd_config_before.log"
    flash_image_by_page "${NORMAL_IMAGE_DEFAULT}" "${NORMAL_IMAGE_SIZE}" \
        "${NORMAL_IMAGE_BASE}" "${NORMAL_ERASE_SIZE}" "${backup_dir}" "flash_normal"

    openocd_capture "${backup_dir}/openocd_postflash_readback.log" \
        "init; reset halt; dump_image {${readback}} ${NORMAL_IMAGE_BASE} ${NORMAL_IMAGE_SIZE}; dump_image {${option_after}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    dump_config "${config_after}" "${backup_dir}/openocd_config_after.log"
    cmp -s "${NORMAL_IMAGE_DEFAULT}" "${readback}" ||
        fail "programmed normal image readback differs; CPU remains halted"
    cmp -s "${config_before}" "${config_after}" ||
        fail "reserved configuration range changed; CPU remains halted"
    [[ "$(sha256_file "${option_after}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes changed unexpectedly; CPU remains halted"
    shasum -a 256 "${config_before}" "${config_after}" "${readback}" \
        "${option_after}" >>"${backup_dir}/SHA256SUMS"
    printf 'PASS: audited normal application programmed and read back exactly.\n'
    printf 'The CPU remains halted. Run boot-normal only after the physical bench is ready.\n'
}

flash_normal_eval() {
    local backup_dir config_before config_after readback option_after
    check_eval_image
    [[ ${WRITE_ACK} -eq 1 ]] || fail "missing --i-understand-this-writes-main-flash"
    [[ "${WRITE_CONFIRMATION}" == "${EVAL_IMAGE_SHA256}" ]] ||
        fail "missing exact --confirm-eval-sha value"
    progress "flash-normal-eval: known-good CAN evaluation image accepted."
    backup_current
    backup_dir="${OUTPUT_DIR}"
    config_before="${backup_dir}/config_before_0x0803C000_4KiB.bin"
    config_after="${backup_dir}/config_after_0x0803C000_4KiB.bin"
    readback="${backup_dir}/normal_eval_readback_${EVAL_IMAGE_SIZE}B.bin"
    option_after="${backup_dir}/option_bytes_after_normal_eval_flash.bin"
    dump_config "${config_before}" "${backup_dir}/openocd_config_before.log"
    flash_image_by_page "${EVAL_IMAGE_DEFAULT}" "${EVAL_IMAGE_SIZE}" \
        "${EVAL_IMAGE_BASE}" "${EVAL_ERASE_SIZE}" "${backup_dir}" "flash_normal_eval"
    openocd_capture "${backup_dir}/openocd_postflash_readback.log" \
        "init; reset halt; dump_image {${readback}} ${EVAL_IMAGE_BASE} ${EVAL_IMAGE_SIZE}; dump_image {${option_after}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    dump_config "${config_after}" "${backup_dir}/openocd_config_after.log"
    cmp -s "${EVAL_IMAGE_DEFAULT}" "${readback}" || fail "evaluation image readback differs; CPU remains halted"
    cmp -s "${config_before}" "${config_after}" || fail "reserved configuration range changed; CPU remains halted"
    [[ "$(sha256_file "${option_after}")" == "${EXPECTED_OPTION_SHA256}" ]] || fail "Option Bytes changed unexpectedly; CPU remains halted"
    printf 'PASS: known-good CAN evaluation image programmed and read back exactly.\n'
    printf 'The CPU remains halted. Run boot-normal-eval only after the physical bench is ready.\n'
}

boot_normal() {
    local readback option_file
    check_normal_image
    make_temp_dir
    readback="${TEMP_DIR}/normal_image_before_boot.bin"
    option_file="${TEMP_DIR}/option_bytes_before_boot.bin"
    openocd_capture "${TEMP_DIR}/openocd_boot_normal.log" \
        "init; reset halt; dump_image {${readback}} ${NORMAL_IMAGE_BASE} ${NORMAL_IMAGE_SIZE}; dump_image {${option_file}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    cmp -s "${NORMAL_IMAGE_DEFAULT}" "${readback}" ||
        fail "target does not contain the audited normal application image"
    [[ "$(sha256_file "${option_file}")" == "${EXPECTED_OPTION_SHA256}" ]] ||
        fail "Option Bytes differ; refusing to boot"
    openocd_capture "${TEMP_DIR}/openocd_reset_run.log" "init; reset run; shutdown"
    printf 'PASS: audited normal application reset and started.\n'
}

boot_normal_eval() {
    local readback option_file
    check_eval_image
    make_temp_dir
    readback="${TEMP_DIR}/normal_eval_image_before_boot.bin"
    option_file="${TEMP_DIR}/option_bytes_before_eval_boot.bin"
    openocd_capture "${TEMP_DIR}/openocd_boot_normal_eval.log" \
        "init; reset halt; dump_image {${readback}} ${EVAL_IMAGE_BASE} ${EVAL_IMAGE_SIZE}; dump_image {${option_file}} ${OPTION_BASE} ${OPTION_SIZE}; shutdown"
    cmp -s "${EVAL_IMAGE_DEFAULT}" "${readback}" || fail "target does not contain the known-good evaluation image"
    [[ "$(sha256_file "${option_file}")" == "${EXPECTED_OPTION_SHA256}" ]] || fail "Option Bytes differ; refusing to boot"
    openocd_capture "${TEMP_DIR}/openocd_reset_run_eval.log" "init; reset run; shutdown"
    printf 'PASS: known-good CAN evaluation image reset and started.\n'
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
        --confirm-inject-sha) [[ $# -ge 2 ]] || fail "--confirm-inject-sha requires a value"; WRITE_CONFIRMATION="$2"; shift 2 ;;
        --confirm-normal-sha) [[ $# -ge 2 ]] || fail "--confirm-normal-sha requires a value"; WRITE_CONFIRMATION="$2"; shift 2 ;;
        --confirm-eval-sha) [[ $# -ge 2 ]] || fail "--confirm-eval-sha requires a value"; WRITE_CONFIRMATION="$2"; shift 2 ;;
        --confirm-factory-sha) [[ $# -ge 2 ]] || fail "--confirm-factory-sha requires a value"; WRITE_CONFIRMATION="$2"; shift 2 ;;
        --i-understand-this-writes-main-flash) WRITE_ACK=1; shift ;;
        --keep-temp) KEEP_TEMP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown argument: $1" ;;
    esac
done

case "${ACTION}" in
    self-test) self_test ;;
    identify) identify ;;
    preflight) preflight ;;
    backup) backup_current ;;
    flash-safe) flash_safe ;;
    flash-inject) flash_inject ;;
    flash-normal) flash_normal ;;
    flash-normal-eval) flash_normal_eval ;;
    boot-safe) boot_safe ;;
    boot-inject) boot_inject ;;
    boot-normal) boot_normal ;;
    boot-normal-eval) boot_normal_eval ;;
    restore-factory) restore_factory ;;
    -h|--help|help) usage ;;
    *) usage; fail "unknown action: ${ACTION}" ;;
esac
