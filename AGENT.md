# Athena CyberDog Motor Firmware — Agent Handoff

Last updated: 2026-08-14 (Asia/Shanghai)

## Objective

Develop a maintainable, safety-first open firmware for the Xiaomi CyberDog
first-generation joint controller (GD32F303 + DRV8323 + absolute magnetic
encoder), using `athena_motorcontrol`/`bgkatz/motorcontrol` as the licensed
software base. Use `fanmyu/dgm-xiaomi` only as an independent hardware
reference and reimplement required behavior in this repository.

The first controlled interface remains the Mini-Cheetah/MIT five-parameter CAN
command. The project does not aim for binary or OTA compatibility with Xiaomi's
factory firmware.

## Repository State

- Local repository: `/Users/choqy/workspace/xiaomi_dog/athena_motorcontrol`
- Active branch: `cyberdog-safe-bringup`
- Baseline commit: `4c443e6aa1341babeb4229dadb221d366d2ce639`
- Baseline commit message: `port to gd32f303, support cyberdog motor`
- Read-only upstream remote `origin`:
  `https://github.com/zbwu/athena_motorcontrol.git`
- User fork remote `fork`:
  `https://github.com/itonyzhang-cmyk/athena_motorcontrol.git`
- Initial safe baseline implementation commit: `803da04`
- First-flash read-only diagnostic implementation commit: `ccf6522`
- User-owned pre-existing modification: `STM32F446RETX_FLASH.ld`. Do not revert,
  overwrite, stage, or commit it unless explicitly requested. It is not the
  GD32 link script selected by the current Makefile.

## Non-Negotiable Safety Rules

1. Never flash a motor/controller without an explicit user instruction for that
   specific test step.
2. Do not erase or modify option bytes/readout protection during normal work.
3. Preserve the verified factory backup under
   `/Users/choqy/workspace/xiaomi_dog/backups/gd32f303ret6_factory_20260812_170113_CST/`.
4. The first custom image must keep DRV8323 gate drive disabled at all times.
5. Read-only peripheral validation must pass before any energized PWM test.
6. PWM, phase order, current-sense polarity, current scaling, and fault shutdown
   must be independently verified before closed-loop FOC.
7. Initial powered tests use a current-limited supply, an unloaded/fixed motor,
   low current limits, and a reachable hardware emergency stop.
8. A failed or incomplete calibration must never overwrite the last known-good
   parameters.

## Current Status

### Verified

- `athena_motorcontrol` is a fork of `bgkatz/motorcontrol` and contains a
  CyberDog/GD32F303 port.
- The code already maps the major Xiaomi peripherals: TIMER0 PWM, DRV8323 SPI,
  AS5047-class encoder SPI, two phase-current ADC inputs, bus voltage, six Hall
  analog inputs, temperatures, UART, CAN, and flash preferences.
- `fanmyu/dgm-xiaomi` independently agrees with most major peripheral pins.
- The Makefile selects `GD32F303RETx_FLASH.ld`, not the modified STM32 link
  script.

### Build

- Homebrew `arm-none-eabi-gcc` 16.2.0 was installed, but that formula does not
  contain Newlib headers/libraries and cannot build this project (`stdio.h`
  missing).
- The Arm official 15.3.rel1 package was downloaded by Homebrew. Its installer
  requires an interactive administrator password, so it was not system-installed.
- For a non-privileged build check, the official package payload was extracted
  under `/tmp/arm-gnu-toolchain-15.3-root` and passed through `GCC_PATH`.
- The official compiler found a Newlib compatibility defect in
  `Core/Src/sysmem.c`: obsolete `caddr_t` use. It was replaced with `void *`.
- The default `SAFE_BRINGUP=1` build now completes successfully with no C
  compiler warnings. GNU ld reports one known bare-metal RWX LOAD-segment
  warning from the inherited linker layout; this remains to be cleaned up.
- Two independent output directories produced byte-identical ELF/HEX/BIN files.

Current first-flash safe build command:

```sh
make BUILD_DIR=/tmp/athena-safe-build \
  GCC_PATH=/tmp/arm-gnu-toolchain-15.3-root/bin -j4
```

Outputs are always profile-isolated under
`/tmp/athena-safe-build/safe/`. An unsafe build made with
`SAFE_BRINGUP=0` goes under `/tmp/athena-safe-build/unsafe/` and cannot share
objects with the safe image. The safe ELF is first linked as `.unverified` and
only renamed to `.elf` after `tools/verify_safe_image.sh` passes; HEX/BIN are
then generated from that verified ELF.

Safe build result:

- Toolchain: Arm GNU Toolchain 15.3.rel1 / GCC 15.3.1
- Source commit: `ccf6522`
- text: 28,912 bytes
- data: 468 bytes
- bss: 16,972 bytes
- ELF SHA-256: `821c411e927787d249b875883ef8f37e214a35e114e2cd34b826d3bcd9afe44e`
- HEX SHA-256: `4468e3d53148958f74e21c8da13cdafcdb50e8b35db882b8d5e3f2a5913bea06`
- BIN SHA-256: `9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82`
- Flash image end: `0x080072C4`
- Reserved configuration range: `0x0803C000..0x0803CFFF`
- Two fresh output roots produced byte-identical ELF/HEX/BIN files.
- Host tests cover ATHENA-DIAG request CRC/bit corruption/response layout and
  AS5047 read-command parity, response parity/EF, and angle wraparound.
- Symbol audit found `safety_force_outputs_off()` and the read-only diagnostic
  handler, and found no Flash/option-byte write, preference load/write,
  gate-enable, PWM duty, FOC, legacy FSM/MIT unpack, or calibration symbols.
- Persistent reviewed artifacts are under
  `/Users/choqy/workspace/xiaomi_dog/artifacts/athena_safe_diagnostic_ccf6522/`.
- `SAFE_BRINGUP=0` also compiles, but that image is explicitly unsafe and was
  produced only as a compile-regression check. It must not be flashed.

### Open hardware gates before any energized PWM

- Confirm the GD32 linker memory map against the exact controller marking. The
  `MEMORY` block uses 512 KiB Flash/64 KiB SRAM and agrees with the DGM Keil
  target, although the copied linker header incorrectly says STM32/128 KiB.
- Resolve PWM phase-to-terminal mapping against current-sense channel order.
- Verify shunt resistance, DRV8323 CSA gain, ADC reference, and bus divider
  before trusting `I_SCALE` or `V_SCALE`.
- Verify encoder model, SPI mode, parity/error-bit handling, and magnetic status.
- Confirm with a meter/scope that the board-level reset interval also holds
  PA11 low before firmware GPIO initialization.

## Milestones and Gates

Detailed priorities and acceptance criteria are in `docs/PROJECT_PLAN.md`.

- M0 — Reproducible baseline build and documented memory map.
- M1 — Read-only diagnostic image; gate driver cannot be enabled.
- M2 — Passive powered-board verification of clocks, UART, CAN, SPI, ADC,
  temperatures, Hall channels, and fault input.
- M3 — Timer/PWM waveform verification with the gate driver disabled.
- M4 — Low-voltage, current-limited phase/current-sense validation.
- M5 — Low-current encoder and motor-parameter calibration with transactional
  flash storage.
- M6 — MIT CAN current/velocity/position control with enforced watchdogs and
  limits.
- M7 — WebUI integration and repeatable single-motor acceptance testing.

## Required Handoff Record

After each material step, update this file with:

- date and active branch/commit;
- exact command or test performed;
- hardware state (controller power, motor connected, supply voltage/current
  limit, gate enabled/disabled);
- observed output and pass/fail result;
- produced artifact and SHA-256 when relevant;
- unresolved risk and the next safe action.

Do not record secrets, access tokens, or private credentials here.

## Session Log

### 2026-08-14 — Project initialization

- Inspected repository history, branch, remote, Makefile, and current dirty
  state.
- Preserved the pre-existing `STM32F446RETX_FLASH.ld` modification.
- Established `cyberdog-safe-bringup` as the working safety branch.
- Started hardware cross-check and firmware safety audits.
- Hardware and safety audits completed; detailed reports are under `docs/`.
- Installed the Homebrew compiler-only formula and extracted the Arm official
  15.3.rel1 package non-privileged for build verification.
- Updated `_sbrk()` to use a portable `void *` return type for current Newlib.
- No controller was connected, erased, or flashed.

### 2026-08-14 — Audit findings accepted

- Major peripheral pins match independently between `athena_motorcontrol` and
  `dgm-xiaomi`.
- P0 blockers: unverified phase/current mapping, unsafe nFAULT/exception paths,
  disabled DRV sense OCP, missing software I/V/T protections, unchecked encoder
  frames, unsafe defaults/calibration, and unreserved flash configuration page.
- Confirmed ADC configuration defect: ADC0 rank 3 is overwritten with channel
  12 while ADC1 rank 3 is never configured; temperature readings are invalid.
- Phase 0 remains strictly read-only with PA11 low and no flash writes.

### 2026-08-14 — Initial SAFE_BRINGUP implementation

- Added default-on `SAFE_BRINGUP=1` build selection.
- Added a central non-SPI shutdown function that pulls PA11 low, disables the
  TIMER0 primary output and clears all three PWM compare values.
- CPU exception and `Error_Handler()` paths now invoke the shutdown function.
- nFAULT low now immediately shuts down and latches a gate-driver fault; rising
  edges no longer auto-clear it.
- Safe build excludes motor, calibration, setup, zero-position and Flash-write
  paths; CAN frames are consumed without state changes.
- TIMER0 PWM channel and primary outputs are disabled in the safe profile.
- Corrected ADC1 rank 3 to channel 12, restoring the intended motor-temperature
  channel mapping pending DMA/on-board validation.
- Reserved two 2 KiB configuration pages at `0x0803C000` in the linker script
  and added an overlap assertion.
- Updated `_sbrk()` for current Newlib and verified both safe and unsafe profiles
  compile.
- Performed no controller connection, erase or flash operation.
- Committed the implementation as `803da04`; handoff documentation followed in
  `8968b03`. Branch `cyberdog-safe-bringup` was pushed to the user's `fork`
  remote without changing its default branch.

### 2026-08-14 — First-flash SAFE_DIAGNOSTIC image

- Added bounded TBE/RBNE/BUSY SPI transfers and explicit AS5047 chip-select
  timing; implemented the AS5047 one-frame pipeline, command/response parity,
  EF rejection, ERRFL/DIAAGC/MAG startup reads, angle-jump rejection, and
  validity/error counters.
- Added bounded ADC inserted-conversion and startup-calibration waits. ADC or
  SPI failure latches a fault while independently keeping PA11/PWM off.
- Safe TIMER0 ISR reinforces hard shutdown at 30 kHz and performs passive ADC
  and encoder sampling at 1 kHz.
- Added ATHENA-DIAG/1 read-only CAN on standard request `0x701`/response
  `0x781`, exact DATA/DLC8/CRC validation, response limiting, and raw snapshot
  counters. Legacy MIT/Xiaomi frames cannot enter a state machine.
- Made UART TX-only at 115200 8N1 and added 1 Hz raw safety/encoder/ADC reports;
  no ISR performs `printf`.
- Safe startup no longer reads or interprets legacy Flash preferences. Existing
  configuration pages remain reserved and unchanged.
- Added build-profile isolation and release gating: safe/unsafe objects cannot
  mix, and no final safe ELF/HEX/BIN is published until symbol/config-range
  audit passes.
- Independent final safety review found zero software P0 blockers for the
  read-only image. This approval does not extend to gate enable, PWM, motor
  motion, or `SAFE_BRINGUP=0`.
- Host tests, two clean reproducible builds, unsafe-then-safe contamination
  regression, and automated image audit passed. No hardware was connected,
  erased, or flashed.
- `fanmyu/dgm-xiaomi` was not re-fetched during this step; its previously
  recorded pin cross-check remains supporting evidence, while the official
  AS5047P data sheet was treated as the protocol authority.

### 2026-08-15 — First-flash and UC12 passive-test tooling

- Added `tools/athena_safe_flash.sh`, a release-specific ST-LINK/OpenOCD tool
  for the reviewed SAFE_DIAGNOSTIC image. It verifies target voltage,
  debug/device word, 512 KiB Flash size, exact Option Bytes, artifact size, and
  SHA-256 before any write.
- The tool creates two independent full-Flash reads before programming, erases
  only `0x08000000..0x080077FF`, programs and reads back exactly 29,380 bytes,
  checks the reserved `0x0803C000..0x0803CFFF` range before/after, and leaves
  the CPU halted. Booting the safe image is a separate hash-checked action.
- A separately gated factory-recovery action accepts only the recorded 512 KiB
  factory image and never writes Option Bytes. It was implemented but not run;
  its use still requires a separate explicit authorization.
- Added `tools/athena_diag_uc12`, a direct libusb UC12 client restricted to
  standard CAN request `0x701` and the four ATHENA-DIAG read opcodes. It offers
  `ping`, `info`, `snapshot`, `watch`, and `export`, and has no arbitrary CAN,
  MIT, enable, calibration, configuration, reset, or Flash command.
- Snapshot/watch/export first require `ATHN` identity and a safe-state check:
  SAFE must be set while PA11, TIMER0 primary output, and all three PWM-channel
  enable bits remain clear. Raw responses and operator-entered supply
  conditions can be recorded to CSV.
- Added `docs/FIRST_FLASH_RUNBOOK.md` and the `host-tools-test` Make target.
- Verification commands passed: `make host-test host-tools-test`, flash-tool
  artifact/range self-test, warning-clean UC12 host build, safe firmware clean
  rebuild, and `verify-safe` symbol/config-range audit.
- The clean rebuild BIN was byte-identical to the reviewed release and retained
  SHA-256 `9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82`.
- Hardware attempts stopped safely: macOS did not enumerate ST-LINK or UC12;
  OpenOCD returned `open failed`, and the UC12 client returned device-not-found.
  No controller was read, halted, erased, programmed, reset, or booted.
- Next safe action: connect ST-LINK to USB, power the fixed/unloaded controller,
  then run read-only `preflight` and the two-read current-state backup. Connect
  UC12 only after the safe image has been programmed, verified, and explicitly
  booted.

### 2026-08-15 — `独板` read-only SWD connection attempt

- Test target label: `独板`; controller board only, no motor connected.
- The controller was externally powered without the planned current-limited
  bench supply. No main-Flash or Option Bytes write was authorized or issued.
- Sandboxed USB access initially made the ST-LINK appear unavailable. A direct
  system-level read confirmed ST-LINK V2J37S7, serial
  `E1007200D0D2139393740544`.
- OpenOCD consistently measured target voltage at approximately 3.193 V, so
  ST-LINK USB, VTref, and common ground are present.
- Normal SWD at 100 kHz, reduced-speed SWD at 10 kHz, and connect-under-reset
  at 10 kHz all failed before target identification with `unable to connect to
  the target`. No chip ID, Flash, or Option Bytes were read.
- The established seven-pin orientation is pin 1 VTref, pin 3 SWCLK, pin 4
  SWDIO, pin 5 NRST candidate, and pin 7 GND. The next action is power-off
  inspection/continuity of pins 3 and 4 and verification that the ST-LINK-end
  SWCLK/SWDIO labels were not crossed. Do not proceed to backup or programming
  until a read-only preflight identifies the Cortex-M4 and 512 KiB Flash.
