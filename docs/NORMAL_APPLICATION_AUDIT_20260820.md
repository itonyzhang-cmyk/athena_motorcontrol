# Normal Application Audit

Date: 2026-08-21

Build profile: `SAFE_BRINGUP=0 BRINGUP_INJECT=0`

## Audited behavior

- Startup configures the DRV8323, then returns to an electrical idle state:
  `PA11=0` and TIMER0 primary output disabled.
- The timer channels may be configured, but no PWM reaches the gate driver
  until a mode-entry preflight succeeds.
- `MOTOR_MODE` and `CALIBRATION_MODE` require no latched safety fault, no DRV
  fault, high nFAULT, valid encoder data, and valid ADC data.
- Gate enable raises PA11 and starts a non-blocking 10 ms charge-pump interval
  while TIMER0 primary output remains disabled. It then reapplies and verifies
  the complete runtime DRV configuration before enabling PWM; no SysTick wait
  occurs inside the TIMER0 ISR.
- nFAULT still forces the common output shutdown path while PA11 is high.
- Startup now performs a DRV8323 readback gate after configuration: all SPI
  transfers must complete, FSR1/FSR2 must be clear, nFAULT must be high, and
  DCR/CSACR/OCPCR must match the configured values. A failure latches the gate
  driver safety fault and leaves the power stage disabled.
- Runtime CSACR keeps calibration disabled and sense overcurrent enabled.
  OCPCR uses the bench-proven `0x0415` value. Shutdown lowers POEN and PA11
  immediately and performs no SPI transaction after EN_GATE is low.
- A reserved Flash slot is accepted as application configuration only when
  its magic, format version, payload length, CRC32, and parameter ranges all
  pass. Factory code remnants and interrupted writes therefore fall back to
  explicit RAM defaults (`CAN_ID=1`, `CAN_MASTER=0`, timeout 1000) instead of
  being interpreted as motor settings. Flash is not modified during fallback.
- Configuration format version 2 uses both reserved 2 KiB pages as sequenced
  A/B slots. A save validates before erasing only the inactive slot, commits
  its magic last, and verifies the complete slot by readback. An interrupted
  save therefore leaves the previous valid slot available at the next boot.
- CAN timeout now zeroes commands, disables the driver, and returns to
  `MENU_MODE`; a fresh explicit motor command is required to re-arm.
- The normal image does not link the `BRINGUP_INJECT` source or inject symbols.

## Static findings and residual limits

The upstream control path remains intentionally present: MIT five-parameter CAN
commands enter `MOTOR_MODE`, then FOC/field-weakening/commutation run at the
timer interrupt rate. Calibration and preference writes are also part of the
normal application and still require separate bench validation. This audit does
not prove phase order, current polarity/scaling, PWM polarity/deadtime, or
mechanical behavior.

## Automated evidence

```text
make host-test host-app-test host-tools-test
  PASS: protocol tests
  PASS: AS5047 protocol tests
  PASS: motor gate preflight tests
  PASS: UC12 tool self-test
  PASS: flash-tool self-test

tools/verify_normal_image.sh ... motorcontrol.elf
  PASS: normal application symbols and no inject path
```

## Release artifact

- BIN: `artifacts/athena_normal_led_heartbeat_20260821/motorcontrol.bin`
- Size: 52,292 bytes
- SHA-256: `5687609beeabd8fea8a4bf4566fd9847589ee0f7e92fd562d9d314ab9388167c`
- ELF SHA-256: `0a5178de4b4adf8fd8ab6a69e8676a70863b64015f021940c8f09c765d7ce3ee`
- HEX SHA-256: `95fabdb7cf1ca906962acadba89c393a1e0a21c66fe5bc1483b2b2cdc216aa97`

This is an audited build candidate, not yet a hardware-accepted release. The
hash-locked flasher provides `flash-normal` and `boot-normal`, but neither is
run automatically.
