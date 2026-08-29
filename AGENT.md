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

- **唯一允许的 Arm GNU 工具链**：项目内固定目录
  `/Users/choqy/workspace/xiaomi_dog/.toolchains/arm-gnu-15.3`（Arm GNU
  Toolchain 15.3.Rel1 / GCC 15.3.1，包含完整 Newlib）。构建禁止引用
  `/tmp` 或 Homebrew 的不完整 `arm-none-eabi-gcc`。
- 构建时必须显式指定：

  ```sh
  make -C athena_motorcontrol \
    GCC_PATH=/Users/choqy/workspace/xiaomi_dog/.toolchains/arm-gnu-15.3/bin \
    SAFE_BRINGUP=0 -j4
  ```

- Homebrew `arm-none-eabi-gcc` 16.2.0 was installed, but that formula does not
  contain Newlib headers/libraries and cannot build this project (`stdio.h`
  missing).
- The Arm official 15.3.rel1 package was downloaded by Homebrew. Its installer
  requires an interactive administrator password, so it was not system-installed.
- The project-local toolchain above is the authoritative build source.
- The official compiler found a Newlib compatibility defect in
  `Core/Src/sysmem.c`: obsolete `caddr_t` use. It was replaced with `void *`.
- The default `SAFE_BRINGUP=1` build now completes successfully with no C
  compiler warnings. GNU ld reports one known bare-metal RWX LOAD-segment
  warning from the inherited linker layout; this remains to be cleaned up.
- Two independent output directories produced byte-identical ELF/HEX/BIN files.

Current first-flash safe build command:

```sh
make BUILD_DIR=build \
  GCC_PATH=/Users/choqy/workspace/xiaomi_dog/.toolchains/arm-gnu-15.3/bin -j4
```

Outputs are always profile-isolated under
`build/safe/`. An unsafe build made with
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

### 2026-08-29 - Motor-side MIT boundary and upper-controller reduction

- The AS5047 magnetic encoder is now the sole firmware position source and
  MIT/FOC boundary: position, velocity, and torque feedback are motor-side.
  `GR` no longer participates in FOC, MIT torque packing, CAN feedback, or
  UART setup; its persisted slot is retained only to load legacy preference
  pages. Hall feedback remains unavailable because the required magnet is not
  fitted.
- The reviewed WebUI normal-image record is now motor-side image
  `artifacts/athena_motor_side_protocol_20260829/motorcontrol.bin`, SHA-256
  `e087eeeb01d76d0562d8200300876fd7ac562b1be96ea6afdf38c220234144a5`.
  It records `output_reduction=9.0`. The high-level trajectory panel accepts
  and displays output-side coordinates and explicitly maps them to motor-side
  MIT fields; the raw custom MIT panel remains motor-side. For example, an
  output target of 4.0 rad maps to a motor-side target of 36.0 rad.
- Full host regression and the normal-image symbol audit passed. On the remote
  bench, the image and configuration hash were transferred successfully, but
  `athena_safe_flash.sh identify/preflight` stopped before any write: ST-LINK
  reported 3.19 V then OpenOCD could not connect to the target. No Flash,
  preference page, or Option Byte was modified. Restore target power/SWD access
  before retrying the hash-locked `flash-normal` and no-enable feedback check.

### 2026-08-29 - Motor-side image flash and non-enable CAN confirmation

- After a repeat preflight passed, target UID `39305137-14303434-47457A29`,
  Option Bytes, and the 3.19 V target-voltage gate were verified. The exact
  `e087eeeb01d76d0562d8200300876fd7ac562b1be96ea6afdf38c220234144a5`
  normal image was programmed through the page-by-page normal flow. The
  mandatory two 512 KiB pre-flash reads matched each other; post-flash image
  readback matched the source exactly; the configuration page was byte-for-byte
  unchanged; and the post-flash Option Bytes retained SHA-256
  `c0b942fbb9fe967ec0e7b675e080d48c930fc5fe3fde70f6dd6f9646fdffc0d3`.
- `boot-normal` passed. Three non-enable MIT frames then returned motor-side
  position `12.0287 rad`, which the WebUI correctly displayed as output-side
  position `1.3365 rad` at the configured 9:1 reduction. No enable (`0xFC`)
  or motion command was sent. The bridge is stopped and both MIT/enable state
  flags are false.

### 2026-08-29 - FWDGT self-test isolation correction and motion checks

- Review found that the original `FWDGT_SELFTEST` branch was entered after
  USART, TIMER0, CAN0, both SPI units, both ADC units, and EXTI initialization.
  Those calls did not enter FOC/FSM or send a CAN frame, but violated the
  intended minimal-isolation boundary. The branch now runs immediately after
  RCU and board-safe GPIO initialization, before all of those peripherals.
- Added `verify-fwdgt` / `tools/verify_fwdgt_selftest_image.sh`. It requires
  only `MX_RCU_Init`, `MX_GPIO_Init`, and `runtime_watchdog_init` in `main()`
  and rejects UART, timer, CAN, SPI, ADC, EXTI, DRV, and FSM startup calls.
  `make FWDGT_SELFTEST=1 ... all` and the full host regression suite pass.
  Corrected self-test image SHA-256:
  `1983291343cd27cfa9ebf3b6b40688764f7f97eaa632c36daf4800b760b0cd34`.
  This corrected image is build/audit-verified but has not yet replaced the
  already hardware-proven earlier self-test image.
- On the remotely running audited normal image
  `86e4705c191c5c735881b74e8e1a9cb2f34a8ce44d72087a24d8730366539789`, a
  `-0.10 rad/s`, 3 s velocity session sent 151 frames with a 29.9 ms maximum
  frame gap and sent `0xFD` on completion. A 3 s S-curve position session
  from 3.7430 to 4.0000 rad (`Kp=3`, `Kd=0.5`) ended at host logical position
  3.9941 rad. A 10 s `-0.05 rad/s` session was manually stopped after 2 s;
  the host recorded `0xFD`, 103 frames, a 29.9 ms maximum gap, and an idle
  (`enable_active=false`) state afterward.
- Post-motion read-only diagnostics show zero DRV fault status and the known
  DCR/CSACR/OCPCR values. Read-only configuration reports `GR=1` and
  `CAN_TIMEOUT=1000`; this board is therefore currently configured in
  motor-side, not 9:1 output-side, units. The velocity feedback/host logical
  displacement was inconsistent with the low requested speeds, so do not use
  these sessions as output-axis velocity acceptance until the gear-ratio
  configuration and velocity-scale evidence are reconciled.

### 2026-08-29 - 20 ms host schedule and closed-loop retest

- Restored the WebUI process with `ATHENA_MIT_INTERVAL_S=0.020` after a
  restart had silently fallen back to the 5 ms default. The CAN bridge was
  recovered by terminating only an orphaned `uc12_slcan_bridge` process that
  held the UC12 device. Three non-enabling MIT frames received feedback before
  any motion command.
- On normal image `86e4705c191c5c735881b74e8e1a9cb2f34a8ce44d72087a24d8730366539789`,
  each 3 s session sent 151 frames with maximum gaps of 29.8-30.0 ms and
  emitted the `0xFD` stop frame. Status afterward was `mit_active=false` and
  `enable_active=false`.
- A position S-curve from 0.0544 to 0.3000 rad (`Kp=3`, `Kd=0.5`, no hold)
  moved the feedback to 0.2474 rad. The 0.0526 rad residual is recorded as a
  no-hold tracking error, not a position acceptance pass.
- Velocity sessions at -0.05, -0.10, and -0.20 rad/s (`Kp=0`, `Kd=1`, 3 s)
  did not produce a measurable net feedback displacement. The velocity-mode
  protocol, timing, feedback, and stop path passed, but output-axis constant
  velocity remains unaccepted pending a low-risk velocity-loop/gear-scale
  investigation.
- Read-only post-stop diagnostics found no latched safety, CAN, SPI, or ADC
  timeout fault. The displayed about -16 A phase values were captured with
  `POEN=0` after a live-PWM CSA zero capture, so they are an off-state
  common-mode mismatch rather than physical winding current. Diagnostics now
  state that boundary explicitly.

### 2026-08-29 - MIT range mismatch correction and output-axis retest

- Root cause of the multi-turn position command was an on-wire MIT range
  mismatch. The controller had persisted `P_MIN=-100`, `P_MAX=100` and
  `GR=9`, while the WebUI still encoded and decoded the same 16-bit field as
  `-12.5..12.5`. Thus a WebUI target of 4 rad decoded in firmware as about
  32 rad, while a real 31.58 rad feedback decoded in the WebUI as about 3.9
  rad. The latter is approximately 5.03 output turns and agrees with the
  observed flange motion.
- The WebUI now owns a persisted MIT position-range record alongside its
  hash-locked normal-image record. Every trajectory, custom MIT command,
  feedback decode, semantic trace, multi-turn unwrap, and 1-degree shortcut
  uses that one range. A committed CAN `P_MIN` or `P_MAX` update also updates
  the local WebUI record only after the controller commit succeeds.
- A no-enable feedback check on the restored `86e470...` normal image decoded
  position `31.5816 rad` with the shared `-100..100` range. No controller
  Flash write occurred. A 3 s output-velocity test at -0.20 rad/s and Kd=5,
  and a nearby 0.20 rad position S-curve at Kp=3, did not move the flange;
  each sent 151/161 frames with a 30.0 ms maximum gap and an explicit `0xFD`.
  This is now attributed to output-axis gain/static-torque calibration, not
  position decoding or CAN timing. Do not restore the old incorrect range to
  obtain motion.

### 2026-08-29 - GD32F303 FWDGT hardware reset verification

- Target controller UID: `39305137-14303434-47457A29`. The FWDGT-only
  acceptance image was written with page-by-page verification and a full-image
  readback match; image SHA-256:
  `c2647455a9d273d49a5e2cf2ce4e990dc2b6d1227a6029b8c6f5c7080d2ae0c7`.
- This image arms FWDGT immediately after safe GPIO initialization, before
  ADC, encoder, DRV, CAN, FSM, or any motor-control path. It sends no CAN
  frames, enters no motion path, and deliberately does not issue a foreground
  watchdog heartbeat.
- An `init; halt` attachment 0.5 s after start observed `FWDGT_PSC=0x00000006`,
  `FWDGT_RLD=0x000001FF`, and `FWDGT_STAT=0x00000000`. After a fresh run for
  8 s, a non-resetting attachment observed `RCU_RSTSCK=0x3C000000` with
  `FWDGTRSTF=1`. Therefore the front-end-stall to GD32 hardware-reset path is
  verified on this board.
- Restoration of the audited normal bench image
  `86e4705c191c5c735881b74e8e1a9cb2f34a8ce44d72087a24d8730366539789` was
  restored remotely using the hash-locked `flash-normal` procedure. Its full
  readback completed with `PASS: audited normal application programmed and
  read back exactly.` The image was then started with `boot-normal`.
- A read-only WebUI status check confirmed the same `normal_sha`,
  `mit_active=false`, and `enable_active=false`. No motion command was sent
  after the restore. The bridge's historical trajectory log is not evidence of
  a post-restore motion command.

### 2026-08-28 - audited unflashed live-CSA candidate

- Rebuilt the normal candidate after synchronizing the default and `-1` fallback
  `CAN_TIMEOUT` to 3000 cycles. `verify-normal` and all host tests pass.
- Candidate artifact: `artifacts/athena_adc_live_offset_rearm_20260828/motorcontrol.bin`,
  SHA-256 `f9cfd8e4bfa1d753bee9d496b81c636b851679443b547e75f62be657fe78679c`.
  It has not been flashed. Do not confuse it with the remotely deployed
  watchdog bench image, whose SHA must be read from the live WebUI before any
  flash or boot operation.

### 2026-08-28 - remote angle and constant-velocity test

- Remote host `192.168.31.20`; WebUI restarted with `~/athena_runtime/.venv/bin/python`; normal image SHA-256 `537cc5e3ee20b835293303c104664f10fc9cfcf517593828d3322a0697519406`.
- CAN trace started and three fixed non-enable MIT checks completed with feedback and no enable action.
- Ran constant velocity `-0.2 rad/s` for `20 s`, `Kp=20`, `Kd=1`, `t_ff=0`, hold `0 s`. Negative direction was required because start angle `11.6470 rad` plus positive 20 s would exceed the `+12.5 rad` protocol limit.
- Angle moved to `7.6568 rad` (`-3.9902 rad` measured vs `-4.0 rad` expected); about `1380` frames; session ended with `enable_active=false`.
- Post-test diagnostics: `safety_fault_latched=0`; DRV FSR1/FSR2 `0x0000`, DCR/CSACR `0x02C000A0`, OCPCR `0x00000415`; `drv_enable_verify_ok=1`, `drv_enable_fsr1_fsr2=0`, `drv_enable_nfault_edge=1`.
- Critical finding: trajectory log reported maximum frame interval `74.1 ms`, above the firmware watchdog threshold of about `33 ms`. This is not a scheduler-pass result and does not justify higher current or wider motion.
- Next gate: fix or isolate remote USB-CAN scheduling jitter, repeat until max frame interval is stably below `33 ms`, keeping current firmware and limits unchanged.

### 2026-08-28 - Python/USB-CAN scheduling A/B test

- Added `ATHENA_MIT_INTERVAL_S` environment override to `tools/athena_bench_webui.py`; default remains `0.005 s`.
- On the same remote host/image and bounded constant-velocity test, measured max gaps: `5 ms -> 74.1 ms`, `10 ms -> 52.8 ms`, `20 ms -> 30.0 ms`.
- The `20 ms` run sent `1001` frames and completed with the bridge stopped afterward. This confirms synchronous PTY -> C bridge -> USB CAN TX/ACK backpressure as the primary timing failure; 5/10 ms are too aggressive for the current bridge, while 20 ms passes the approximately `33 ms` watchdog threshold with limited margin.

### 2026-08-20 - encoder-startup image bench result

- Flashed `artifacts/athena_inject_encoder_startup_20260820/motorcontrol.bin`
  (SHA-256
  `ba9dafc0fb6585558f2981f1298e5b86f3ef6109d6ccb9d10882828b2da19955`).
- Immediately after flashing, the controller LED was off and `ping` did not
  respond; after a controller power cycle, CAN and LED operation resumed. This
  matches the flash tool's documented CPU-halted post-write state.
- After power cycle, `snapshot` passed with `SAFE=1`, `PA11=0`, `POEN=0`,
  `enc=1`, `adc=1`, `passive=PASS`, `safety_fault_latched=0`, zero SPI/ADC,
  encoder parity/EF/jump errors, and zero CAN errors.
- The bounded encoder startup delay/retry fix is therefore effective in the
  no-debugger cold-start path. Next gate is one bounded `drv-wake` test with
  the ST-LINK disconnected and the current-limited bench ready.

### 2026-08-20 - no-debugger startup fallback image

- Further bench evidence: with ST-LINK fully disconnected, LED and CAN stopped;
  source audit found the selected GD32 `system_clock_120m_hxtal()` could halt
  forever when HXTAL did not assert stable. The previously edited generic
  HXTAL helper was not on the active path and was corrected.
- Active 120 MHz HXTAL startup now falls back to the existing 120 MHz IRC8M
  PLL path on HXTAL timeout. No PWM or DRV enable behavior changed.
- New reviewed image: `artifacts/athena_inject_clock_fallback_20260820/motorcontrol.bin`
  (33508 bytes, SHA-256
  `b42993500eda792774bd39ad0ae3cd2557810a8b1dd4c84fb7a6e79e4bc73c90`).
- The safe-flash script is hash- and size-locked to this image; image audit,
  host tests, and flash-script self-test passed.
- Next safe action: flash this exact image, power-cycle with ST-LINK fully
  disconnected, and verify LED plus `ping`/`snapshot` before any `drv-wake`.

### 2026-08-20 - standalone IRC startup image

- Because the fallback image still showed no CAN after full ST-LINK removal,
  the active GD32 clock selection was changed to direct IRC8M PLL at 120 MHz;
  this removes the external HXTAL startup path entirely for the next isolated
  test while preserving peripheral clock assumptions.
- New image: `artifacts/athena_inject_irc_startup_20260820/motorcontrol.bin`,
  33308 bytes, SHA-256
  `06e1f7c107fd158b1532c90b7b7145714c96af3d827a298cca02d9a41fa4e637`.
- Image audit, host tests, and flash-script self-test passed. LED behavior is
  not a valid standalone proof in BRINGUP_INJECT because the main loop can
  continue before its legacy LED block; CAN `ping` is the required proof.

### 2026-08-20 - DRV fault-read diagnostic image

- `drv-wake` left `safety_fault_latched=0x20` (`SAFETY_FAULT_GATE_DRIVER`),
  proving nFAULT fell immediately after ENABLE and the prior implementation
  skipped the SPI reads, leaving `0xFFFF` defaults.
- Added a bounded, PWM/POEN-disabled fault-read window: the EXTI handler records
  nFAULT during this window without forcing PA11 low, the service performs one
  complete 8-frame SPI capture, then always disables PA11 and reports the wake
  unavailable unless nFAULT and all register checks pass.
- New image:
  `artifacts/athena_inject_drv_fault_read_20260820/motorcontrol.bin`, 33332
  bytes, SHA-256
  `6e1eb4e5397d95de802b3f225a2163d80604effb0570d7c35df5f3606d9213bd`.
- Host tests, image audit, and flash-script self-test passed. No PWM output was
  added.

### 2026-08-20 - standalone DRV wake verification

- After a full power cycle with ST-LINK disconnected, `drv-wake` returned
  `status=OK payload=1`; snapshot page 27 reported `drv_ready=1`.
- FSR1/FSR2 were zero, DCR/CSACR/OCPCR matched `0x00A0/0x02DC/0x0415`, all
  eight SPI transfers completed with valid RX words, and passive safety flags
  passed (`PA11=0`, `POEN=1`, `enc=1`, `adc=1`).
- No-debugger cold-start and DRV read/configuration gates are proven. Further
  work requires physical bench confirmation before any bounded PWM/injection
  test.

### 2026-08-20 - DRV SPI fault read and readiness latch result

- On the fault-read image, `drv-wake` returned `status=OK payload=1` and real
  DRV values: FSR1/FSR2 `0x0000`, DCR `0x00A0`, CSACR `0x02DC`, OCPCR `0x0415`;
  all eight SPI RX words were valid. This proves DRV SPI and ENABLE sequencing
  are functional.
- The following snapshot showed `drv_ready=0` only because read-only diagnostic
  frames cleared the readiness latch. Fixed `inject_handle_can()` so opcodes
  0..3 preserve a successful wake result.
- New image:
  `artifacts/athena_inject_drv_ready_latch_20260820/motorcontrol.bin`, 33340
  bytes, SHA-256
  `bb92730f4bf6e46655ec8817891d73e7994d883590784ab2e4181a7390600c1a`.
- Host tests, image audit, and flash-script self-test passed. No PWM output was
  added; the next manual step is to flash and verify `drv_ready=1` after the
  wake command.

### 2026-08-20 - ST-LINK-independent encoder startup audit

- Bench comparison: with the ST-LINK board cable removed but the controller
  powered, the MCU LED and CAN remained alive while `enc=0`; removing the
  ST-LINK USB power stopped the board entirely. This is treated as a startup
  sequencing/debugger-timing symptom, not evidence of incorrect hardware.
- Updated `Core/Src/main.c` to allow 100 ms for the AS5047P power-up path before
  the first read-only sample.
- Updated `Core/Src/position_sensor.c` so encoder warm-up retries up to three
  times with 20 ms gaps. A transient warm-up failure no longer latches a safety
  fault before the bounded retries finish; final failure still latches the
  appropriate encoder/SPI fault, and DRV/PWM remain disabled.
- Host protocol tests and `tools/athena_safe_flash.sh self-test` passed.
- A new firmware artifact/SHA was not produced in this session because the
- A complete Arm GNU 15.2.rel1 toolchain was restored under `/tmp` and used to
  build two isolated, byte-identical BRINGUP_INJECT outputs. The reviewed BIN
  is `artifacts/athena_inject_encoder_startup_20260820/motorcontrol.bin`,
  SHA-256 `ba9dafc0fb6585558f2981f1298e5b86f3ef6109d6ccb9d10882828b2da19955`.
- `tools/athena_safe_flash.sh` now defaults to and hash-locks this image.
- Next safe action: user flashes this exact image, repeats the two ST-LINK cable
  states, and confirms `enc=1` before any `drv-wake` request.

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

### 2026-08-16 — `独板` verified backup and factory-image comparison

- The prior failure was confirmed to be crossed/incorrect SWD wiring. After
  correction, the target returned Cortex-M4 DAP ID `0x2BA01477`. The generic
  OpenOCD `stm32f1x` target defaults to the Cortex-M3 ID, so the guarded tool
  now explicitly supplies the GD32F303 M4 DAP ID before loading that target
  template.
- Updated the preflight register output to explicit machine-checked fields.
  It passed at target voltage 3.193 V with CPU Cortex-M4 r0p1, debug/device
  word `0x17010414`, Flash size `0x0200` KiB, `OBSTAT=0x03FFFFFC`, and
  `WP=0xFFFFFFFF`.
- Created the read-only board-specific backup at
  `/Users/choqy/workspace/xiaomi_dog/backups/gd32f303ret6_duban_preflash_20260816_01/`.
  Both independent 512 KiB reads are byte-identical with SHA-256
  `0840735c361384f96f3674a6acf4d4833890388660fd459ce3586033f8eef981`.
  Option Bytes match the earlier board byte-for-byte, SHA-256
  `c0b942fbb9fe967ec0e7b675e080d48c930fc5fe3fde70f6dd6f9646fdffc0d3`.
- `独板` and the 2026-08-12 factory backup are not full-image identical:
  49,518 bytes differ across 37 physical pages. Their active motor application
  range `0x08002000..0x08033FFF` is byte-identical (`0.2.4A`, Git
  `7b844b0fM`, built 2021-03-23 09:39:01).
- The active bootloaders are both version `0.1.5` but have different build
  dates. `独板` carries an older `0.2.3` fallback application at `0x08034000`,
  while the earlier board carries `0.2.4A`. Board-specific encoder/Hall
  calibration data also differs at `0x0807D000..0x0807E5C9`.
- Recovery must use each board's own full backup. The detailed comparison is
  stored beside the `独板` binaries as `COMPARISON_WITH_FACTORY_20260812.md`.
- This entire operation was read-only. No main Flash or Option Bytes erase or
  program command was issued. The original firmware was reset to run after
  each read, and no motor was connected.

### 2026-08-16 — per-board UID identification

- Read the factory-programmed 96-bit device UID from `0x1FFFF7E8..0x1FFFF7F3`
  over SWD without modifying the target. `独板` is registered as
  `39305137-14303434-47456052`, formatted as
  `UID[95:64]-UID[63:32]-UID[31:0]`.
- Added the read-only `tools/athena_safe_flash.sh identify` action. Preflight
  and future backup metadata now also capture the UID automatically.
- The local board registry is
  `/Users/choqy/workspace/xiaomi_dog/backups/BOARD_REGISTRY.md`.

### 2026-08-18 — original-motor-connected board identification

- Identified the board connected to the original motor through ST-LINK V2J37S7
  using a halt-only SWD session. UID: `39305137-14303434-47457A29`
  (`UID[95:64]..UID[31:0]`), which is distinct from `独板`.
- The target reported 3.214 V, debug/device word `0x17010414`, and 512 KiB
  Flash. It is now bound to the double-read 2026-08-12 factory backup
  `gd32f303ret6_factory_20260812_170113_CST/` and designated as the next safe
  first-flash target.
- No main Flash or Option Bytes write was issued. The CPU remains halted;
  do not reset/run it until the bench supply and first-flash conditions are
  reconfirmed.

### 2026-08-18 — SAFE_DIAGNOSTIC first flash on original-motor board

- Bench conditions: 12 V controller supply, 0.2 A current limit. Target UID was
  rechecked as `39305137-14303434-47457A29` before the write.
- Pre-flash backup: `/Users/choqy/workspace/xiaomi_dog/backups/gd32f303ret6_preflash_20260818_113741_CST/`;
  two complete 512 KiB reads were byte-identical (SHA-256
  `378978bafac4a3e04454c4ff135b8f342dc5c04809c2197cafa2ecebf6d962ab`).
- Programmed image: `artifacts/athena_safe_diagnostic_ccf6522/motorcontrol.bin`,
  SHA-256 `9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82`.
  Only `0x08000000..0x080077FF` was erased/programmed; readback matched
  exactly. Reserved config and Option Bytes were unchanged.
- Result: flash verification passed; CPU remains halted. `boot-safe` and CAN
  ping/snapshot are the next separate actions and have not been performed.

### 2026-08-18 — factory image recovery after erase timeout

- User authorized restoration of the board-specific factory image. The initial
  full-chip erase timed out before programming completed; the application was
  observed erased (`0xFF`) and the target was kept halted.
- Recovery was completed from the same hash-locked 512 KiB image in sixteen
  32 KiB erase/write blocks. Each block passed `verify_image` with no error.
- Verification directory:
  `/Users/choqy/workspace/xiaomi_dog/backups/factory_restore_verification_20260818_123605_CST/`.
  Option Bytes readback matched the expected SHA-256
  `c0b942fbb9fe967ec0e7b675e080d48c930fc5fe3fde70f6dd6f9646fdffc0d3`.
- CPU remains halted. Do not run factory firmware until the user explicitly
  authorizes the separate boot/test step.

### 2026-08-18 — factory CAN physical-link verification

- After factory boot and correction of the CANH/CANL orientation, the read-only
  `tools/uc12_discover/uc12_discover --send --motor-id 1` test succeeded at
  1 Mbps.
- Received extended response `0x000001FE` with payload
  `29 7A 45 47 34 34 30 14`, identifying motor ID 1 and MCU UID
  `297A454734343014`.
- This confirms the UC12 adapter, bus power/termination path, CAN transceiver,
  and current CANH/CANL direction. Do not swap the lines again.

### 2026-08-16 — BRINGUP_INJECT gated single-phase injection profile

- User decision: probing PA8/PA9/PA10/PA11/PA12 on the controller is not
  practical because the board is dense and conformal-coated, so the logic
  analyzer gate is replaced by a gated low-current injection bring-up. The
  motor, encoder, and phase-current ADC are the instruments. The user accepted
  the small residual risk that a config error could cause a brief current
  event before the supply folds back, bounded by the 0.2 A bench limit and the
  firmware watchdog.
- New build profile `BRINGUP_INJECT=1` (introduced `e44465e`, finalized
  `89b6629`):
  - Default state is passive: PA11 low, all PWM compares at the all-low
    position, no current path.
  - DRV8323 configured with PA11 low: 3x PWM, COAST clear (PA11 is the only
    power gate), VDS OCP latched, sense OCP enabled, gate faults enabled.
  - CAN opcode `0x04` arms one pulse with vector (0..5), duty
    (0.5..5.0 %), duration (10..50 ms), all from fixed tables. Opcode `0x05`
    stops immediately. Read-only diag opcodes are unchanged.
  - The 30 kHz timer ISR performs the hardware sequencing and aborts on
    deadline, nFAULT, latched fault, PA11 drop, or ADC deviation above the
    ~2 A watchdog limit.
  - Symbol audit (`tools/verify_inject_image.sh`) forbids Flash writes,
    Option Bytes, calibration, legacy FSM, MIT unpack, and unsafe DRV config;
    requires the inject state machine, bounded SPI/ADC helpers, and the
    reserved configuration range.
- UC12 client gained `inject VECTOR DUTY DURATION --confirm-inject`, `stop`,
  `drv`, and snapshot pages 20..26 (status, signed peak currents, encoder
  start/end, ticks/faults, DRV registers).
- Flash tool gained hash-locked `flash-inject` and `boot-inject` actions.
  `flash-inject` performs the two-read current-state backup, erases only
  `0x08000000..0x08007FFF`, programs and reads back the 31,940-byte image,
  verifies the reserved config range and Option Bytes, and leaves the CPU
  halted.
- Verified results:
  - Safe image rebuilt byte-identical, SHA-256 still
    `9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82`.
  - Inject image: 31,940 bytes, SHA-256
    `98cac3d5bb76601b82214e349a5dba63b41218910b3a052168f207037d9f6de2`,
    image end `0x08007CC4`, stored under
    `/Users/choqy/workspace/xiaomi_dog/artifacts/athena_inject_bringup_89b6629/`.
  - Host protocol tests, UC12 self-test, flash-tool self-test, and inject
    symbol audit all pass.
- Bench procedure: `docs/INJECT_BRINGUP_RUNBOOK.md`. Hardware state at the end
  of this step: no flash write performed; `独板` still runs factory firmware;
  the 12 V current-limited supply is available; motor bench not yet set up.
- Next action requires explicit user authorization: connect motor + bench
  supply, then `flash-inject`, `boot-inject`, passive checks, and the six-
  vector injection matrix at 0.5 % / 10 ms.

### 2026-08-18 — CAN verified: root cause was the host tool, not firmware

- Restored the board-specific 512 KiB factory backup in 16 x 32 KiB
  erase/write/verify blocks (single full-chip erase times out at 100 kHz
  SWD). Whole-flash readback SHA-256 matched
  `302f25ed7848ec22c77dbce177c79f548b6de502be71f06976034f8df9cb1ec7`;
  Option Bytes unchanged (`c0b942fb...`). CPU was kept halted throughout.
- Ran the factory firmware briefly and captured live GPIO/CAN registers:
  factory drives PB10 output-push-pull 2 MHz high and PB12 at 50 MHz; the
  safe image left PB10 floating. Live full-GPIO parity to factory did NOT
  change CAN behavior, ruling out a transceiver enable-pin difference.
- TX beacon probe proved MCU -> UC12 (and mailbox/TX) fully working; the
  real failure was `athena_diag_uc12` requiring `confirmation[2] & 0x80`
  after a CAN TX, while the proven `uc12_discover` path only requires
  `confirmation[2] >= 1`. The stricter check rejected every valid UC12 TX
  confirmation and produced the misleading "UC12 did not confirm".
- Fixes kept in the workspace:
  - `tools/athena_diag_uc12/athena_diag_uc12.c`: TX confirmation check now
    matches `uc12_discover`; read-only queries (opcode 0..3) retry once if
    the firmware 20 ms rate limiter drops the request.
  - `Core/Src/can.c`: factory CAN timing (prescaler 4, BS1 10 TQ, BS2 4 TQ,
    auto bus-off, retransmit enabled).
  - `Core/Src/gpio.c`: PB9 = AF push-pull 50 MHz, matching factory
    `GPIOB_CTL1=0x949342B8` nibble 0xB (GD32 mode; factory is NOT open-drain).
- Verified safe diagnostic firmware (SAFE_DIAGNOSTIC, gate off):
  - 29,380 bytes, SHA-256
    `96f03a7a64a6f450734f5bc851731691d556f9e7869243d7ee35420eb34aee52`,
    stored under
    `/Users/choqy/workspace/xiaomi_dog/artifacts/athena_safe_diagnostic_factory_can_96f03a7a/`;
    registered as the default safe image in `tools/athena_safe_flash.sh`.
  - CAN results: `ping` returns `ATHN`; `info` pages 0..4 OK; `snapshot`
    pages 0..19 OK; `watch` counter pages 9..13 OK. Safety flags:
    `SAFE=1 PA11=0 POEN=0 CH=000 CAN_ERR=0`.
  - Firmware counters: every accepted request received a response
    (`rx_valid == tx_submit`); the few observed timeouts were the
    firmware's 20 ms response rate limiter under burst polling, now
    absorbed by the tool's single retry.

### 2026-08-19 — BRINGUP_INJECT flashed and verified

- User explicitly authorized `flash-inject` after the motor bench was
  prepared. The tool's missing `--confirm-inject-sha` parser case was added;
  no erase/write occurred during the initial rejected invocation.
- Target UID `39305137-14303434-47457A29` and target voltage `3.216 V` passed
  preflight. A fresh two-read 512 KiB backup was created under
  `/Users/choqy/workspace/xiaomi_dog/backups/gd32f303ret6_preflash_20260819_103523_CST/`;
  both reads matched with SHA-256
  `9432474f75e4066b732d15a625f25301a8f98d04e5cacfd81e26dff34e66c7a7`.
- Programmed the reviewed `BRINGUP_INJECT` image
  (`98cac3d5bb76601b82214e349a5dba63b41218910b3a052168f207037d9f6de2`) only
  into `0x08000000..0x08007FFF`. The 31,940-byte readback matched exactly;
  reserved config and Option Bytes were unchanged.
- CPU remains halted. No `boot-inject` or injection pulse has been issued.
  Next safe action is passive bench boot and `ping`/`drv`/`snapshot` checks.

### 2026-08-19 — BRINGUP_INJECT CAN receive-path rebuild

- Passive diagnostics after `boot-inject` timed out. This was not the prior
  UC12 transmit-confirmation parser bug: `athena_diag_uc12` already accepts
  the validated transmit confirmation condition (`confirmation[2] >= 1`) and
  retries one read-only request after the 20 ms response limiter.
- Root cause in the old `89b6629` inject image: `BRINGUP_INJECT=1` forces
  `SAFE_BRINGUP=0`, while `can_rx_init()` and `can_tx_init()` selected the
  ATHENA-DIAG filter/response defaults only for `SAFE_BRINGUP`. The hardware
  therefore filtered diagnostic request `0x701` as if it were motor ID 1.
- Corrected only these two selection conditions so `BRINGUP_INJECT` shares the
  diagnostic receive-all hardware filter and `0x781`/DLC8 response defaults.
  The change does not alter PA11 gating, the fixed 0.5..5% / 10..50 ms pulse
  tables, DRV setup, current watchdog, calibration exclusion, or reserved
  Flash configuration range.
- Rebuilt twice in isolated output trees with xPack Arm GNU Toolchain 15.2.1;
  ELF/HEX/BIN were byte-identical. `make host-test host-inject-test
  host-tools-test` and `verify-inject` passed. The one inherited RWX LOAD
  segment linker warning remains.
- Published the hash-locked candidate image at
  `artifacts/athena_inject_bringup_can_fix_20260819/motorcontrol.bin`:
  31,980 bytes, SHA-256
  `9c3003c28a65eb601cf5d918f3a748e730bd3f3cef2d7c2a72fe2333339e42fe`,
  image end `0x08007CEC`; configuration remains reserved at
  `0x0803C000..0x0803CFFF`. No Flash write, boot, CAN injection, or motor
  movement was performed for this rebuild. Reflashing this replacement image
  is a separate explicit authorization.

### 2026-08-19 — paged BRINGUP_INJECT flash procedure

- Replaced the one-shot `flash-inject` application-area erase/write with a
  fixed 16 x 2 KiB page procedure. Each page is erased, written when it
  contains image bytes, and verified in an independent OpenOCD invocation.
  The partial final image page is verified for its programmed bytes while the
  rest of the approved application erase range remains erased.
- The procedure retains its existing pre-write double 512 KiB backup,
  target/Option-Byte checks, post-write complete image readback, reserved
  configuration-range comparison, and Option-Byte hash validation. A page
  failure stops immediately with the CPU halted; it does not continue to later
  pages or reset/run the target.
- No hardware connection, Flash write, boot, CAN injection, or motor movement
  was performed while adding this procedure.

### 2026-08-19 — gated DRV wake/configuration verification image

- Investigated the `drv` readback of `0xFFFF` with PA11 low. It is not treated
  as a usable driver state and injection remains rejected until a separate
  `drv-wake` request verifies the driver configuration.
- Added CAN opcode `0x06`, requiring the host-side `--confirm-drv-wake` flag.
  It disables TIMER0 primary output, sets all comparisons low, raises PA11 for
  a bounded 1 ms, writes/reads the restricted DRV configuration, then always
  drops PA11 before it replies. Boot performs no DRV SPI writes.
- `drv_ready` is set only for clean FSR1/FSR2 plus DCR `0x00A0`/`0x00A1`
  (self-clearing fault-clear bit), CSACR `0x02DC`, and OCPCR `0x0415`.
  `inject` explicitly refuses all requests until that condition holds.
- Added snapshot pages 27..30 for the latched wake verification values and
  updated the UC12 tool/runbook. Two isolated xPack 15.2.1 builds were
  byte-identical; host protocol/tool tests and the inject symbol audit passed.
- Published hash-locked candidate
  `artifacts/athena_inject_bringup_drv_wake_20260819/motorcontrol.bin`:
  32,428 bytes, SHA-256
  `76726989c1f2a0af5e62f0b10e58fe34ae06a281dd1c789857189cbfc094bcd1`,
  image end `0x08007EAC`. `tools/athena_safe_flash.sh` now selects this image.
  No controller was flashed, booted, or commanded during this change.

### 2026-08-19 — DRV wake CAN-ISR deadlock fix

- Hardware evidence: the first `drv-wake` request produced no command response,
  and all subsequent `ping` requests timed out. Root cause: its 1 ms
  `delay_1ms()` ran in the CAN RX interrupt, whose priority prevents SysTick
  from decrementing the delay counter. The MCU remained in that ISR and PA11
  could remain asserted. The operator was instructed to remove motor power.
- Reworked opcode `0x06`: CAN RX now only records the request and enters a
  PWM-disabled wake window; `inject_service()` in the main loop waits for the
  SysTick deadline, performs the bounded DRV SPI transaction, unconditionally
  drops PA11, restores all-low PWM idle, and then sends the response. TIMER0
  maintains PWM disabled during the wake window. Stop/unrelated frames cancel
  the pending wake safely.
- Added host-only `drv-wake-status` for reading pages 27..30 without reissuing
  the PA11 wake request. It cannot recover a deadlocked old image; that image
  must be replaced while motor power is off.
- Two isolated xPack 15.2.1 builds are byte-identical. Host tests and inject
  symbol audit passed. Published hash-locked candidate
  `artifacts/athena_inject_bringup_drv_wake_isr_fix_20260819/motorcontrol.bin`:
  32,604 bytes, SHA-256
  `78226f6d833cd315d7de167790cf660d5b127d53a1989ad4804e507e0ee38c1f`,
  image end `0x08007F5C`. `tools/athena_safe_flash.sh` selects it. No agent
  flash, boot, CAN command, or motor movement was performed for this fix.

### 2026-08-19 — DRV wake PA11 output-latch check fix

- Hardware result from the ISR-fix image: opcode `0x06` returned without
  deadlocking, but reported `UNAVAILABLE`, `drv_ready=0`, and all four latched
  DRV values as zero. This establishes that the SPI transaction was not entered;
  it is not evidence of a zero-valued DRV register set.
- Cause: `inject_service()` additionally required PA11's GPIO output-data
  latch to read high before starting SPI. On this board that readback was not a
  reliable proof of the physical enable state, so it blocked the transaction
  after the bounded wake interval.
- Removed only that output-latch predicate. The wake still requires nFAULT
  high, disables TIMER0 primary output and holds all compares low, waits one
  millisecond in the main-loop state machine, then unconditionally pulls PA11
  low before responding. `drv_ready` remains gated on clean FSR values and an
  exact restricted configuration readback; `inject` remains unavailable unless
  `drv_ready=1`.
- Two isolated xPack 15.2.1 builds were byte-identical; `verify-inject` and
  host protocol/tool tests passed. Published the unflashed candidate
  `artifacts/athena_inject_bringup_drv_wake_latch_fix_20260819/motorcontrol.bin`:
  32,588 bytes, SHA-256
  `ae23073ca5871fa5fe0545f0c9f7ab6f018155c4e72d263e0381cb421615276b`,
  image end `0x08007F4C`. `tools/athena_safe_flash.sh` now selects it. No agent
  Flash write, boot, CAN command, or motor movement was performed for this fix.

### 2026-08-19 — DRV wake hardware-ready interval correction

- The PA11 output-latch fix was flashed and its image readback matched
  `ae23073...`, but `drv-wake` still returned `UNAVAILABLE` with all latched
  SPI values zero. PA12/nFAULT was high (the diagnostic prints `nFAULT=0` for
  a non-asserted active-low input), so the transaction reached its SPI section.
- The bounded interval had been set to 1 ms. The existing non-bring-up
  `drv_init_config()` path documents and uses a 10 ms delay after PA11 rises
  before its first DRV SPI write. One millisecond can sample the DRV before
  its digital interface is ready, explaining a zero MISO readback.
- Restored the 10 ms hardware-ready interval. TIMER0 primary output remains
  disabled and all compares remain all-low for the entire interval; PA11 is
  still unconditionally pulled low before the response. No injection is
  permitted without the same strict `drv_ready=1` readback gate.
- Two isolated xPack 15.2.1 builds were byte-identical; published the
  unflashed candidate
  `artifacts/athena_inject_bringup_drv_wake_settle_fix_20260819/motorcontrol.bin`:
  32,588 bytes, SHA-256
  `1040c4ca322fb4b457a708543edb46951d399c8147749f62a59db04bb3878cf8`,
  image end `0x08007F4C`. `tools/athena_safe_flash.sh` now selects it. No agent
  Flash write, boot, CAN command, or motor movement was performed for this fix.

### 2026-08-19 — DRV SPI frequency and chip-select timing correction

- The 10 ms wake image was flashed and reached the SPI transaction: latched
  values changed from zero to `FSR1/FSR2/DCR/CSACR/OCPCR = 0xFFFF`. This is a
  no-response MISO pattern, not a clean DRV configuration or a `drv_ready`
  pass. Injection remains blocked.
- `MX_SPI1_Init()` used APB1/8. With the GD32 120 MHz clock profile APB1 is
  60 MHz, so the DRV bus ran at 7.5 MHz, above the DRV8323's 5 MHz SPI limit.
  Changed only SPI1 (DRV) to APB1/16 = 3.75 MHz; encoder SPI2 is unchanged.
- Added deterministic CS setup/hold delays around every DRV SPI transfer using
  the existing bounded NOP helper. The wake still holds PWM disabled/all-low,
  always lowers PA11 before replying, and still requires full exact readback
  before it reports `drv_ready=1`.
- Two isolated xPack 15.2.1 builds were byte-identical; published the
  unflashed candidate
  `artifacts/athena_inject_bringup_drv_spi_rate_fix_20260819/motorcontrol.bin`:
  32,644 bytes, SHA-256
  `4bb4c470b2fc502ce5e4fe94e0128eeed992a4bff7e7266c166bf6c27beb72ad`,
  image end `0x08007F84`. `tools/athena_safe_flash.sh` now selects it. No agent
  Flash write, boot, CAN command, or motor movement was performed for this fix.

### 2026-08-19 — DRV SPI evidence capture after persistent all-high readback

- The rate/timing image (`4bb4c470...`) was flashed and still returned clean
  SPI words of `0xFFFF` for every DRV register. This is explicitly not treated
  as a configuration success and `drv_ready` remains zero; no injection is
  authorized.
- Rechecked the DRV8323 Revision D datasheet, section 7.6/page 19. It specifies
  `tCLK >= 100 ns` (10 MHz maximum), not the earlier undocumented 5 MHz claim.
  The existing 3.75 MHz SPI1 setting remains conservative, but its former
  5 MHz comment was corrected and frequency is no longer presented as a proven
  root cause.
- Added a one-shot evidence record to the existing bounded `drv-wake` command:
  all eight SPI TX/RX words, each TBE/RBNE/BUSY completion result, SPI1
  CTL0/CTL1/STAT, and GPIO A/B control/output/input snapshots are latched on
  pages 31..48 before PA11 is dropped. RX is initialized to `0xFFFF` before
  every transfer; `drv_ready` now additionally requires all eight transfers to
  report `SPI_TRANSFER_OK`.
- The host `drv-wake` command prints all evidence pages in one run. It still
  raises PA11 for only the bounded 10 ms all-low/PWM-disabled window and always
  lowers it before responding. It does not add PWM output, inject commands, or
  Flash writes.
- Two isolated xPack 15.2.1 builds produced byte-identical ELF/HEX/BIN;
  protocol, host-tool, flashing-tool self-tests, and the inject symbol/config
  audit passed. Published the unflashed candidate
  `artifacts/athena_inject_bringup_drv_spi_evidence_20260819/motorcontrol.bin`:
  33,300 bytes, SHA-256
  `0eb86f727f82dba3451854a8d4dde068d4cd329c5b613d712a8349795d856ea0`,
  image end `0x08008214`. The hash-locked flash tool now selects this image.
- The image exceeds the former 16-page/32 KiB application range. The paged
  flasher was therefore extended to exactly 17 x 2 KiB pages
  (`0x08000000..0x080087FF`), still well below the immutable configuration
  reservation at `0x0803C000`. Its self-test verifies this non-overlap.
  No controller Flash write, boot, CAN command, or motor movement was
  performed during this change.

### 2026-08-19 — SPI1 mapping cross-check and nSCS high-time correction

- The evidence image showed all eight SPI transfers completed (`page 31 = 0`),
  TX words were correct, and every RX word was `0xFFFF`. PB13/PB15 were
  alternate-function outputs, PB14 was a high input, and PA12/nFAULT was high.
- A proposed SPI1 AFIO-remap change was rejected at compile time because this
  GD32 library exposes no `GPIO_SPI1_REMAP`. Source and the independent
  hardware audit confirm that SPI1 is natively PB13/PB14/PB15; only encoder
  SPI2 is remapped to PC10/PC11/PC12. No unbuildable remap code was retained.
- Rechecked the DRV8323 section 7.6 timing sequence: it requires nSCS to stay
  high for at least 400 ns between frames. The previous transfer helper delayed
  after nSCS low and before nSCS high but not after the high transition. Added
  the existing deterministic delay after every nSCS high transition. This is a
  genuine protocol correction; it does not establish the root cause until one
  bounded wake is tested. PA11/PWM/injection behavior is unchanged.
- Two isolated xPack 15.2.1 builds, host tests, image audit, and flasher
  self-test passed. Published candidate
  `artifacts/athena_inject_bringup_drv_cshigh_20260819/motorcontrol.bin`:
  33,324 bytes, SHA-256
  `d4dea6e94f024bcbfb2e8d61b0dca338aadc0087d3e5528328833e86d4908802`.
  No controller was flashed or commanded after this rebuild.

### 2026-08-19 — DRV SDO input bias correction candidate

- Re-audited the all-`0xFFFF` result under the assumption that the board
  wiring and DRV8323RS variant are correct. The firmware configured PB14/SDO
  as `GPIO_MODE_IN_FLOATING`, while DRV8323 SDO is open-drain and releases the
  line between response bits. Changed only PB14 to `GPIO_MODE_IPU`; SPI mode,
  frame format, timing, PA11 window, PWM shutdown, and `drv_ready` validation
  are unchanged.
- This is an unverified candidate. It must be tested with one bounded
  `drv-wake`; no injection is allowed unless the exact readback passes.
- Two isolated builds, host tests, image audit, and flasher self-test passed.
  Candidate image:
  `artifacts/athena_inject_bringup_drv_sdo_ipu_20260819/motorcontrol.bin`,
  33,324 bytes, SHA-256
  `9bde97ea0923cdc57ac33ca43ccac0272359d751603fc3f419e0bea4d5a892f5`.
  No controller was flashed or commanded for this candidate.

### 2026-08-20 — drv_ready clear-reason page dispatch correction

- Page 49 (`drv_ready_clear_reason`) was implemented in `inject_snapshot()`,
  but the diagnostic dispatcher forwarded only pages 20 through 48. Therefore
  `page=49 status=BAD_PAGE` was a firmware dispatch omission, not evidence of
  a failed flash or unsupported diagnostic version.
- Added only the missing page-49 dispatch case. The page reports why the
  readiness latch was last cleared: 1 boot/reset, 2 new wake, 3 explicit stop,
  or 4 unknown control frame. No PWM, wake sequencing, injection acceptance,
  or Flash-writing behavior changed.
- Published the unflashed candidate
  `artifacts/athena_inject_drv_ready_reason_page49_20260820/motorcontrol.bin`:
  33,380 bytes, SHA-256
  `eaf40e726236aa4c3f3762e4fa5509a4648ba03c9ce0e75009fdab0769ee32a5`.
  The hash-locked flash script selects this image. Build image audit and
  host/host-tool/flasher self-tests passed before bench use. No controller
  Flash write, boot, CAN command, or motor movement was performed for this fix.

### 2026-08-20 — nFAULT edge qualification after controlled disable

- Bench evidence: `drv-wake` completed with `drv_ready=1` and zero DRV status
  faults, but the immediate `inject 0 0.5 10` request was refused with
  `payload=0x20` (`SAFETY_FAULT_GATE_DRIVER`). This is a firmware sequencing
  issue, not a failed DRV wake: `inject_service()` intentionally lowers PA11
  after the verified wake; the DRV then normally asserts nFAULT because it is
  disabled, and EXTI incorrectly latched that post-disable edge as a gate
  fault.
- The nFAULT ISR now records a gate-driver fault only while PA11 is high. It
  retains the bounded wake-window fault capture with PWM/POEN disabled, and
  real nFAULT assertions during an active injected pulse still force immediate
  shutdown and fault latching.
- Published the unflashed candidate
  `artifacts/athena_inject_drv_nfault_disable_edge_20260820/motorcontrol.bin`:
  33,396 bytes, SHA-256
  `8f0c90c211a8153ca20ba9d7fd02c242398c383d05abaea11f967a50458b2cf5`.
  The hash-locked flash script selects this image. No controller Flash write,
  boot, CAN command, or motor movement was performed for this fix.

### 2026-08-20 — pre-clear DRV fault capture

- The first bounded injection was accepted but aborted at 2 PWM ticks with
  `SAFETY_FAULT_GATE_DRIVER (0x20)`. Added a diagnostic-only recovery path:
  when that is the sole MCU-latched fault, the next `drv-wake` may enter the
  existing PWM/POEN-disabled window even if nFAULT is low.
- The wake service now reads DRV FSR1/FSR2 before any DCR write containing
  `CLR_FLT`; pages 50 and 51 expose those pre-clear values. All other safety
  faults still reject wake, and no PWM is enabled by this recovery path.
- Published unflashed candidate
  `artifacts/athena_inject_drv_prefault_capture_20260820/motorcontrol.bin`:
  33,460 bytes, SHA-256
  `34df802ff94bf4af3ac5ce4c67d6d3b5b011831994c033510bfdba463fe7a366`.
  Hash-locked flasher, image audit, host tests and diagnostic-tool tests pass.
  No controller Flash write, boot, CAN command, or motor movement was
  performed for this change.

### 2026-08-20 — post-abort DRV fault capture

- The pre-clear FSR1/FSR2 pages returned zero after the aborted pulse, so the
  next diagnostic must capture the DRV status at the actual nFAULT edge. Added
  pages 52/53 for a bounded post-abort FSR1/FSR2 read with PWM disabled; the
  first-tick and later-tick nFAULT paths both set the capture request.
- Published unflashed candidate
  `artifacts/athena_inject_drv_fault_capture_20260820/motorcontrol.bin`:
  33,612 bytes, SHA-256
  `ab2105e69da4638eea029c104b07c5c391a89b8d2a0ebc20bb501d1440405a8e`.
  No controller Flash write, boot, CAN command, or motor movement was
  performed for this diagnostic-only change.

### 2026-08-20 — injection charge-pump precharge

- Post-abort DRV evidence decoded as `FSR1=0x0400` (global FAULT) and
  `FSR2=0x0040` (CPUV). The injection path previously disabled PA11 after
  `drv-wake`, then re-enabled it only on the first PWM tick. That allowed a
  switching edge before the DRV8323 charge pump had settled.
- Injection now enters a 10 ms precharge state after command acceptance: PA11
  is high while PWM primary output is disabled and all three switching inputs
  are all-low. It enables PWM only after nFAULT, PA11 and all latched safety
  checks pass. The requested pulse duration and 0.5% minimum duty are
  unchanged; stop/unknown traffic still aborts precharge.
- Published unflashed candidate
  `artifacts/athena_inject_drv_precharge_20260820/motorcontrol.bin`:
  33,780 bytes, SHA-256
  `5eb1af4e31da09efbe400b519c33e63676f11349f4d65d11fe02fd05eb2c586a`.
  No controller Flash write, boot, CAN command, or motor movement was
  performed for this timing correction.

### 2026-08-20 — strict idle output-off state

- Tightened the post-wake and post-injection idle invariant to PA11 low and
  TIMER0 primary output disabled (`POEN=0`), with all three compare registers
  at their all-low value. The former `POEN=1` idle condition was electrically
  benign with PA11 low but did not meet the stricter bench precondition.
- The nFAULT ISR already latches `SAFETY_FAULT_GATE_DRIVER` only when PA11 is
  high, while preserving the controlled wake-window diagnostic read. Injection
  still enables POEN only after its 10 ms PA11-high charge-pump precharge and
  safety recheck; any completion, stop, or fault returns to strict idle.
- Published unflashed candidate
  `artifacts/athena_inject_strict_idle_20260820/motorcontrol.bin`: 33,588
  bytes, SHA-256
  `ca40e8d1e9c0f286e19b26644288d9159561c953fc9f32dfe67565c47602e68d`.
  No controller Flash write, boot, CAN command, or motor movement was
  performed for this safety-state correction.

### 2026-08-20 — POEN-only staged injection diagnostic

- Added a bounded intermediate `POEN_TEST` state after the existing 10 ms
  PA11-high charge-pump precharge. TIMER0 primary output is enabled while all
  three compare values remain all-low for 10 ms; DRV FSR1/FSR2 are captured on
  pages 54/55 before the real PWM vector is allowed.
- A nonzero POEN-stage FSR or any safety fault aborts before phase PWM. The
  existing nFAULT shutdown and post-abort pages 52/53 remain unchanged.
- Published unflashed candidate
  `artifacts/athena_inject_poen_stage_20260820/motorcontrol.bin`: 33,788
  bytes, SHA-256
  `dad1a6343156c6528829990daf4c0d56d6b03186527b97d8a45ae0588564c639`.

### 2026-08-20 — normal application audit candidate

- Audited the `SAFE_BRINGUP=0 BRINGUP_INJECT=0` path. Startup now returns to
  PA11 low with TIMER0 primary output disabled; MOTOR/CALIBRATION entry checks
  latched safety faults, DRV fault, nFAULT, encoder validity and ADC validity.
- Gate enable raises PA11, waits for nFAULT, clears DRV coast, then enables the
  TIMER0 primary output. CAN timeout now disables the driver and returns to
  MENU_MODE, requiring a fresh explicit motor command.
- Added the pure host-tested `motor_gate_check()` policy and a normal-image
  symbol audit; no BRINGUP_INJECT source or symbols are linked in the normal
  image.
- Published unflashed candidate
  `artifacts/athena_normal_app_audit_20260820/motorcontrol.bin`: 48,940 bytes,
  SHA-256 `e94eefe1c7052b20e2bb53796199c3f729cd8c93645b1794784bfd05d1812b4b`.
- `make host-test host-app-test host-tools-test`, normal symbol audit, and
  flash-tool self-test passed. No controller Flash write, boot, CAN motor
  command, or motor movement was performed.

### 2026-08-20 — normal application LED heartbeat correction

- Bench boot of the first normal candidate showed no LED blinking. Static
  audit found `status = ~status`, which changed the `FlagStatus` value to
  `0xFFFFFFFF` after the first loop and prevented further toggling.
- Replaced it with an explicit `RESET`/`SET` toggle and rebuilt the candidate.
- Updated normal application BIN SHA-256 to
  `4d8e4d2a50a0f3310f552e9cd0dced61632a443d775073e45adad134c97c3ef5`.
  The previous normal image must not be used for the next boot test.

### 2026-08-20 — SysTick heartbeat during normal startup

- The normal candidate's main-loop LED fix was present, but its heartbeat only
  ran after DRV/encoder startup. Added a 500 ms PC13 toggle in `SysTick` so the
  LED also proves that the CPU is executing during peripheral initialization.
- Published replacement candidate SHA-256
  `820df55b32701b3efd09ed41690b45f4443fa29728a9050a69ebeca5d3136598`.
  The earlier `4d8e...` image must not be used for the next test.

### 2026-08-20 — normal CAN receive filter correction

- The normal application still used the legacy GD32-incompatible hardware CAN
  mask (`CAN_ID << 5`) that had already been removed from BRINGUP_INJECT.
- Normal CAN now accepts frames in the hardware filter and applies exact
  standard/data/DLC8/`CAN_ID` validation in the RX ISR, matching the proven
  diagnostic receive path.
- Published replacement candidate SHA-256
  `4fdd467d97996fb570715bb4304785c4baf85c7295531aaefb66a564a75c8830`.
  The previous `820df...` image must not be used for CAN testing.

### 2026-08-20 — normal DRV configuration/readback gate migration

- The bring-up wake path had already demonstrated that successful SPI writes
  alone were insufficient: every transfer must complete, FSR1/FSR2 must be
  clear, nFAULT must be high, and DCR/CSACR/OCPCR must read back as configured.
- The normal `drv_init_config()` path now performs that same pipelined DRV8323
  readback before returning to strict idle. Any timeout, fault, low nFAULT, or
  mismatched register latches `SAFETY_FAULT_GATE_DRIVER` and keeps MOTOR_MODE
  unavailable. Injection-only pages/state machines remain excluded.
- Updated normal candidate:
  `artifacts/athena_normal_app_audit_20260820/motorcontrol.bin`, 49,252 bytes,
  SHA-256 `2d986c368d40564890179d81f543edbe361ec3c38c473c727904a332eb873137`.
  Offline host tests, normal symbol audit, and flash-tool self-test passed.
  No controller Flash write, boot, CAN motor command, or motor movement was
  performed for this build.

### 2026-08-20 — normal configuration format and CAN-ID fallback correction

- Bench evidence showed the normal image received no matching `0x001` frame.
  The preserved page at `0x0803C000` began with factory instruction/string
  words, including `CAN_ID=0x000D0A6C`; the old loader rejected only `-1` and
  therefore treated those words as application settings.
- Added an application-owned configuration format with `ATHN` magic, version,
  payload length, CRC32, and range validation across CAN and motor parameters.
  Invalid/legacy pages now select explicit RAM defaults (`CAN_ID=1`,
  `CAN_MASTER=0`, timeout 1000) without modifying Flash.
- Configuration saving validates before erase and writes the commit magic
  last. Invalid or interrupted saves cannot replace the last valid page.
- Updated unflashed normal candidate is 51,220 bytes, SHA-256
  `3f7c245cb1b22f9ee37ccd637a4afb20557f1ce7e1ed3992202d7d660b74cbd1`.
  Normal/safe/inject builds, configuration host tests, normal symbol audit,
  and flash-tool self-test passed. No controller Flash write or CAN command
  was performed for this correction.

### 2026-08-21 — normal motor-entry sequencing correction

- Re-auditing the normal application found an ISR deadlock regression:
  `drv_enable_gd()` waited on SysTick while called from the 30 kHz TIMER0 ISR.
  It also wrote OCPCR `0x0455` while its readback gate required the bench-proven
  `0x0415`, so the normal startup gate could never pass.
- Motor entry is now a non-blocking 10 ms charge-pump precharge with PA11 high,
  POEN disabled and no commutation. After the interval it reapplies the full
  runtime DRV configuration, verifies SPI completion/register readback/nFAULT,
  and only then enables POEN.
- The runtime CSA configuration disables calibration and enables sense OCP.
  Disable/timeout paths lower POEN and PA11 immediately and no longer attempt
  SPI access after EN_GATE is low.
- Configuration format v2 now uses the two reserved 2 KiB pages as sequenced
  A/B slots. New settings are written and readback-verified in the inactive
  slot while the previous valid slot remains intact, satisfying the retained
  configuration requirement under reset or power loss.
- A replacement normal artifact must be built and hash-locked before flashing;
  the prior `3f7c245c...` image is obsolete.
- Two isolated normal builds were byte-identical. Published unflashed artifact:
  `artifacts/athena_normal_app_audit_20260821/motorcontrol.bin`, 52,372 bytes,
  SHA-256 `538da9dc8aac7c4e144de6d6b8f126fbc8f7fee348ef1976ed6b9a6e72330afa`;
  image end `0x0800CC94`, below configuration base `0x0803C000`.

### 2026-08-21 — normal LED heartbeat ownership correction

- The normal application drove PC13 from SysTick, its one-second main loop,
  and the 30 kHz TIMER0 ISR. Those writers race, so the visible heartbeat was
  not a reliable startup indication despite the known PC13 board mapping.
- PC13 is now initialized low and is owned solely by the 1 kHz SysTick path,
  which alternates the output every 500 ms. The symmetric toggle remains
  visible for either board LED polarity and does not affect CAN, DRV enable,
  PWM, or motor state.
- Published unflashed normal artifact:
  `artifacts/athena_normal_led_heartbeat_20260821/motorcontrol.bin`, 52,292
  bytes, SHA-256
  `5687609beeabd8fea8a4bf4566fd9847589ee0f7e92fd562d9d314ab9388167c`.
  Host tests, normal symbol audit, and flash-tool self-test passed. No
  controller Flash write, boot, CAN motor command, or motor movement was
  performed for this correction.

### 2026-08-27 - CAN configuration migration

- Commit `63fd1de` exposes transactional `fsm_save_preferences()` shared by
  UART Setup and CAN configuration.
- CAN diagnostic opcode `0x07` on ID `0x701` stages fields in RAM: writes use
  `0x20 + field*4 + byte`, reads use `0x90 + field*4 + byte`; `0xF8` validates,
  `0xF9` commits, and `0xFA` aborts. Commit requires menu state and no fault.
- Stable field IDs 0..20 cover phase/CAN/zero settings and all current,
  motor, position, velocity, gain, and temperature settings.
- CLI supports `config get`, `config set FIELD VALUE [--commit]`, `config commit`,
  and `config abort`; WebUI exposes the CAN path while retaining UART Setup.
  Candidate build uses the project-local Arm GNU 15.3 toolchain and is not
  flashed.

### 2026-08-28 - multi-node CAN / upgrade design preflight

- Audited the normal MIT route: `CAN_ID` is the receive ID and legacy-named
  `CAN_MASTER` is the feedback transmit ID. The persisted validation now
  accepts the full standard 11-bit range and rejects fixed ATHENA-DIAG
  `0x701/0x781` collisions; no configuration was written to hardware.
- Added the read-only/offline `tools/athena_can_topology.py` preflight and
  unit tests. It catches ID ownership collisions before connecting multiple
  controllers. `docs/MULTI_NODE_AND_UPDATE_PLAN.md` records the staged
  multi-node acceptance test and A/B application-upgrade design.
- Verified locally with `make host-app-test host-can-topology-test` and a
  normal-image `verify-normal` build using the pinned Arm GNU 15.3 toolchain.
  No controller Flash write, boot, CAN transmission, second controller, or
  motor movement was performed.

### 2026-08-28 - independent runtime watchdog and opt-in I/V/T trip paths

- Added a GD32 FWDGT runtime backend after TIMER0/NVIC startup.  TIMER0 reloads
  it only after observing foreground-loop progress, so a stalled foreground
  loop, control ISR, or permanently executing higher-priority ISR leads to a
  reset rather than a continuing heartbeat.  A configuration failure latches
  a fault and prevents motor entry.  The roughly 3.3-second nominal timeout
  has not yet been measured on hardware or verified through reset-cause data.
- Added opt-in, host-tested current, bus-voltage, and temperature trip logic.
  Defaults remain disabled: the installed board's current/bus scales are not
  yet characterized, and the application has no validated physical
  temperature source.  Once explicitly enabled, invalid source data or a
  threshold trip disables outputs and remains latched until reset.
- Full host tests plus normal/safe/inject image verification completed using
  the pinned Arm GNU 15.3 toolchain.  No candidate image was flashed, booted,
  or sent any CAN command in this work.
