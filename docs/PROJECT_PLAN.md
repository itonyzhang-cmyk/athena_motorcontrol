# Athena CyberDog Open Motor Firmware — Project Plan

## 1. Project Goal

Create our own auditable motor-controller firmware for the Xiaomi CyberDog
first-generation joint control board while retaining the proven control
architecture and MIT license of `bgkatz/motorcontrol`.

The firmware should eventually provide:

- configurable pole pairs, encoder direction/offset, phase order, gear ratio,
  CAN ID, current limits, and control limits;
- MIT five-parameter position/velocity/impedance/torque control over 1 Mbps CAN;
- reliable calibration with validation and recoverable flash storage;
- current, bus voltage, temperature, encoder, gate-driver, CAN-timeout, and
  watchdog protection;
- clear diagnostics and a protocol suitable for the existing local WebUI;
- a repeatable single-motor acceptance test for the user's motor inventory.

## 2. Source Strategy

### Software base

- `bgkatz/motorcontrol`: upstream control architecture and MIT CAN behavior.
- `zbwu/athena_motorcontrol`: existing GD32F303/CyberDog port and our immediate
  development base.

### Hardware references

- `fanmyu/dgm-xiaomi`: independent reference for Xiaomi peripheral mapping and
  board constants.
- Factory firmware backup/static analysis: reference for original memory map,
  peripheral behavior, board revision, and safe restoration.
- Component datasheets: final authority for GD32F303, DRV8323, encoder, CAN, and
  analog scaling behavior.

`dgm-xiaomi` has no explicit license, so do not copy its implementation into
this MIT repository. Use hardware facts as evidence and write our own code.

## 3. Priority Definitions

- **P0 — Safety/blocking:** required before any energized PWM or motor motion.
- **P1 — Core function:** required for a usable single-motor firmware.
- **P2 — Engineering quality:** required before repeated testing or multiple
  motors.
- **P3 — Enhancement:** useful after the fundamental control path is stable.

## 4. Work Breakdown

### P0 — Baseline, hardware truth, and fail-safe behavior

- [ ] Add the user's GitHub fork as the writable remote without replacing the
  read-only upstream relationship.
- [ ] Install/pin an Arm GNU embedded toolchain and document its version.
- [x] Produce a reproducible SAFE_BRINGUP ELF/HEX/BIN and record hashes.
- [x] Verify GD32F303RET6 flash/SRAM sizes and reserve the existing flash
  preference pages, and absence of overlap.
- [x] Establish the initial cross-referenced board pin/peripheral matrix.
- [ ] Resolve PWM U/V/W ↔ physical phase ↔ ADC current-channel mapping.
- [ ] Verify PWM frequency, center alignment, polarity, deadtime, ADC sample
  instant, and control-loop `DT` consistency.
- [ ] Verify DRV8323 SPI mode/register setup, EN_GATE polarity, nFAULT behavior,
  CSA gain, OCP mode, and safe power-up/power-down sequence.
- [ ] Verify AS5047-class encoder type, SPI mode, resolution, parity/error bits,
  diagnostic registers, and angle direction.
- [ ] Verify current-sense and bus-voltage scaling from schematic/measurements.
- [ ] Complete ADC DMA layout validation; the confirmed ADC1 rank typo is fixed.
- [x] Implement a compile-time `SAFE_BRINGUP` profile that cannot enable the
  gate driver from boot, UART, CAN, calibration, or fault recovery.
- [x] Add initial fault latching and a single shutdown function that forces
  gate disable and zero duty.
- [ ] Add independent CAN timeout and hardware watchdog behavior.
- [ ] Add encoder-invalid, overcurrent, over/undervoltage, and overtemperature
  shutdown paths.
- [ ] Prevent empty/corrupt flash from creating aggressive default limits.

### P1 — Passive diagnostics and controlled motor bring-up

- [ ] Add UART diagnostics for firmware ID, reset reason, clock frequencies,
  gate state, DRV registers/faults, encoder raw/diagnostics, ADC raw values,
  converted voltage/current/temperature, Hall channels, and CAN counters.
- [ ] Add a read-only CAN diagnostic/heartbeat frame separate from motor enable.
- [ ] Validate the diagnostic image on one controller with no motor motion.
- [ ] Verify timer PWM on a scope while DRV gate output remains disabled.
- [ ] Verify current-sense zero offsets and noise with PWM disabled.
- [ ] Perform low-voltage/current-limited phase injection to establish phase
  order and current polarity.
- [ ] Implement bounded, abortable encoder/pole-pair calibration.
- [ ] Store calibration transactionally with version, length, CRC, and previous
  known-good fallback.
- [ ] Set conservative first-test defaults for current, speed, torque, Kp/Kd,
  voltage, and temperature.
- [ ] Validate current-loop stability before enabling velocity or position loops.
- [ ] Validate MIT enable/disable/zero and five-parameter commands.
- [ ] Verify CAN loss always disables torque within the specified deadline.

### P2 — WebUI, test automation, and maintainability

- [ ] Add a new backend protocol profile for the custom MIT firmware without
  disturbing factory-Xiaomi protocol support.
- [ ] Expose firmware identity/config/calibration state and raw fault details in
  the WebUI.
- [ ] Implement a guided single-motor acceptance test with CSV/raw-CAN export.
- [ ] Add unit tests for CAN packing, limit conversions, flash CRC/versioning,
  encoder wraparound, and calibration validation.
- [ ] Add host-side static analysis and warning-clean build targets.
- [ ] Add release metadata, versioning, build provenance, and artifact hashes.
- [ ] Document recovery to the verified factory backup.

### P3 — Later enhancements

- [ ] Evaluate six analog Hall channels for startup redundancy and fault
  diagnosis.
- [ ] Add configurable output-side/motor-side units and gear-ratio reporting.
- [ ] Add bootloader/update strategy only after the application is stable.
- [ ] Evaluate additional CAN protocols or ROS/robot integration.
- [ ] Design a production/self-built controller variant independently of the
  Xiaomi PCB.

## 5. Milestone Acceptance Criteria

### M0 — Reproducible baseline

- A pinned toolchain builds from a clean checkout with no errors.
- ELF memory usage and vector/reset addresses are documented.
- Generated BIN/HEX hashes are reproducible.
- No flash operation is part of the default build target.

### M1 — Read-only diagnostic image

- All code paths leave EN_GATE inactive and PWM outputs at a harmless state.
- UART/CAN motor commands cannot transition into MOTOR or CALIBRATION modes.
- The image can report MCU/peripheral status without writing calibration/config.
- A build-time and run-time banner clearly says `SAFE_DIAGNOSTIC`.

### M2 — Passive powered validation

- Supply voltage agrees with a meter within the defined tolerance.
- Encoder angle is stable and follows hand rotation with no parity/errors.
- DRV8323 identity/config/fault registers read consistently.
- Current offsets/noise and both temperature channels are plausible.
- CAN receives/transmits at 1 Mbps without enabling the power stage.

### M3 — PWM timing validation

- Scope confirms frequency, complementary relationship/3-PWM mode, polarity,
  duty bounds, deadtime behavior, and ADC trigger timing.
- Gate driver remains disabled throughout this milestone.

### M4 — Phase/current validation

- Bench supply is current limited and test voltage/current are recorded.
- Each commanded phase vector produces the expected physical phase response and
  measured current sign.
- Any timeout, encoder fault, nFAULT, or emergency stop immediately removes
  drive.

### M5 — Calibration

- Calibration motion/current bounds are explicit before execution.
- Pole pairs, encoder direction/offset/LUT and phase order pass plausibility
  checks.
- Power interruption or calibration failure preserves the old configuration.
- Stored configuration survives reset and reports a valid CRC/version.

### M6 — Closed-loop MIT control

- Current loop is stable before velocity/position tests.
- MIT commands are range checked and use motor-side units consistently.
- CAN timeout and explicit stop produce deterministic torque-off behavior.
- Conservative point/jog tests are repeatable under both no-load and known load.

### M7 — WebUI and acceptance test

- The UI identifies factory Xiaomi versus custom MIT firmware unambiguously.
- A guided test records firmware version, ID, calibration, motion response,
  current, temperature, faults, and raw frames.
- Test output is exportable and sufficient to accept/reject one motor.

## 6. Initial Execution Order

1. Finish hardware and safety audits.
2. Fix the build toolchain and memory map.
3. Add `SAFE_DIAGNOSTIC` compile-time isolation.
4. Implement diagnostic reporting and compile-time tests.
5. Review the resulting diff before any flash discussion.
6. Flash only after the user explicitly approves the exact artifact and test
   setup.
