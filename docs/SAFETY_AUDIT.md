# Firmware Safety Audit and Phase-0 Gate

Date: 2026-08-14

Status: current baseline is **not approved for energized PWM or calibration**.

## P0 Findings

1. `HardFault`, memory/bus/usage faults and `Error_Handler()` do not force the
   power stage off. A CPU exception can leave the last PWM active.
2. The nFAULT ISR clears EXTI before testing it and only records/prints the
   fault; it does not unconditionally pull PA11 low or latch a system fault.
3. CAN timeout only zeroes commands while FOC and PWM remain active. Timeout can
   also be disabled or set to unsafe values.
4. ADC and SPI polling loops have no timeout. A stuck peripheral inside the
   real-time ISR can preserve the last PWM indefinitely.
5. Encoder frames are not validated for parity/error, magnetic health, freeze,
   jump or timeout.
6. Software overcurrent, bus over/undervoltage and temperature shutdowns are
   absent/inactive. DRV sense OCP is disabled in the final register setup.
7. Flash preferences have no magic/schema/length/CRC/calibration-valid marker.
   Erased or corrupt flash can still lead to guessed 40 A limits and 5 A
   calibration current.
8. Calibration enables the driver without sufficient movement/current/time
   validation and writes Flash from the high-frequency control context.

Primary code locations:

- `Core/Src/gd32f30x_it.c:71`
- `Core/Src/gd32f30x_it.c:243`
- `Core/Src/main.c:492`
- `Core/Src/fsm.c:61`
- `Core/Src/fsm.c:113`
- `Core/Src/foc.c:81`
- `Core/Src/position_sensor.c:97`
- `Core/Src/drv8323.c:116`
- `Core/Src/calibration.c:17`
- `Core/Src/flash_writer.c:5`

## Phase-0 Read-Only Acceptance Gate

### Build and image

- Pinned complete Arm GNU toolchain.
- Clean reproducible ELF/HEX/BIN, map, size and SHA-256.
- Linker reserves configuration pages and asserts no overlap.
- Build profile excludes FMC erase/program and all MOTOR/CAL/ZERO paths.

### No power output

- From reset through arbitrary UART/CAN input and injected faults, PA11 remains
  low.
- TIMER0 main output is disabled and PA8/PA9/PA10 stay at a defined harmless
  state.
- Diagnostic operation creates no holding torque or heating.

### Read-only diagnostics

- DRV registers/faults have timeout and validity status.
- Encoder has parity/error/magnetic/jump/freeze checks.
- ADC current offsets, voltage, temperatures and Hall channels have validity,
  error count and timestamp.
- Missing/stale/saturated values are never treated as valid measurements.

### Communication and fault injection

- CAN accepts only the diagnostic protocol's exact ID/type/DLC.
- Short DLC, RTR, extended frames, wrong IDs, random data and floods never
  change output state or Flash.
- UART overlong/random input cannot overflow buffers or enable output.
- CAN loss, bus-off, encoder disconnect, nFAULT, ADC/SPI timeout, CPU fault and
  watchdog reset all leave PA11 low.
- Faults latch and require an explicit clear after all prerequisites pass.

### Parameters and Flash

- Hash the configuration page before/after Phase 0; it must be byte-identical.
- Invalid/missing configuration only permits diagnostic mode.
- All numeric settings reject NaN, Inf, zero where invalid, negative values and
  values beyond hard physical bounds.

Phase 1 may begin only after every Phase-0 item passes on the bench and the
result is recorded in `AGENT.md`.
