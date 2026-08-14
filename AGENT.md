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

Safe build command used:

```sh
make BUILD_DIR=/tmp/athena-safe-build \
  GCC_PATH=/tmp/arm-gnu-toolchain-15.3-root/bin -j4
```

Safe build result:

- Toolchain: Arm GNU Toolchain 15.3.rel1 / GCC 15.3.1
- text: 27,248 bytes
- data: 468 bytes
- bss: 16,836 bytes
- ELF SHA-256: `ed930acba21385bfef601ef750effc72f06d078098b93c7cac9f586e5985990f`
- HEX SHA-256: `f05e1ba423a9914baa905d3ed59dd90c1e4eee9900f5a7309851789d34701b63`
- BIN SHA-256: `5120e67615d98a96591c0db2b1f6fc4d58e8612c5be3f87013f32c2ffd4f201b`
- Flash image end: `0x08006C44`
- Reserved configuration range: `0x0803C000..0x0803CFFF`
- Symbol audit found `safety_force_outputs_off()` and found none of the
  dangerous write/drive symbols (`fmc_page_erase`, `fmc_word_program`,
  `preference_writer_flush`, `drv_enable_gd`, `torque_control`, `commutate`, or
  calibration routines) in the linked safe image.
- `SAFE_BRINGUP=0` also compiles, but that image is explicitly unsafe and was
  produced only as a compile-regression check. It must not be flashed.

### Open P0 Questions

- Confirm the GD32 linker memory map against the exact controller marking. The
  `MEMORY` block uses 512 KiB Flash/64 KiB SRAM and agrees with the DGM Keil
  target, although the copied linker header incorrectly says STM32/128 KiB.
- Resolve PWM phase-to-terminal mapping against current-sense channel order.
- Verify shunt resistance, DRV8323 CSA gain, ADC reference, and bus divider
  before trusting `I_SCALE` or `V_SCALE`.
- Verify encoder model, SPI mode, parity/error-bit handling, and magnetic status.
- Audit default gate-enable behavior and make the diagnostic build incapable of
  enabling PWM, including from UART/CAN commands.

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
