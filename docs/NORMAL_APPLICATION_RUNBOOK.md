# Normal Application Candidate Test Cases

This runbook is for the audited normal application candidate. It does not
authorize a hardware write by itself.

## Offline tests

```sh
make host-test host-app-test host-tools-test
make SAFE_BRINGUP=0 BRINGUP_INJECT=0 \
  BUILD_DIR=/tmp/athena-normal-audit \
  GCC_PATH=/Users/choqy/toolchains/arm-gnu-toolchain-15.2/bin -j4
tools/verify_normal_image.sh \
  /Users/choqy/toolchains/arm-gnu-toolchain-15.2/bin/arm-none-eabi-nm \
  /tmp/athena-normal-audit/unsafe/motorcontrol.elf
tools/athena_safe_flash.sh self-test
```

Expected results: all host tests pass, the normal image audit reports `PASS`,
and the flasher self-test reports `PASS`.

## Hardware test cases

Use the fixed motor, 12 V supply, and 0.2 A current limit already selected for
the bench. The user must perform the physical write/boot actions:

1. `flash-normal` with the exact SHA printed in the audit report; confirm the
   readback and unchanged configuration/Option Bytes.
2. `boot-normal`; verify that LED/CAN startup occurs without motion, holding
   torque, or a current-limit event. Ordinary MIT clients must not send
   application traffic until the calibration gate below has passed. Use the
   exclusive `athena_diag_uc12` diagnostic path for the calibration command;
   it must be the only client owning the UC12 channel for that operation.
   An uninitialized/legacy configuration page is expected to select the RAM
   defaults: receive ID `0x001`, reply ID `0x000`, and timeout 3000 cycles.
3. Recalibrate the flashed normal image before any MIT or trajectory test. Do
   not retain `E_ZERO` or the encoder LUT from a prior image merely because the
   normal flashing path preserves the configuration page:

   ```sh
   ./tools/athena_diag_uc12/athena_diag_uc12 \
     --channel 0 --timeout-ms 1500 control calibrate 2.0
   ```

   Wait for completion, then verify diagnostics page 95 shows `done_ordering=1`
   and `done_cal=1` with no failure (the packed completion value is
   `0x03000000`); page 96 reports the expected `PPAIRS`; pages 97/113 show a
   nonzero `E_ZERO`; page 114 reports the configuration CRC; and page 115
   reports the LUT checksum. Record the exact artifact SHA, board UID,
   calibration current, and pages 95 through 115. Re-run `boot-normal` and
   verify pages 111 through 115 are unchanged, with no safety, DRV, or
   ADC-timeout fault. Calibration, successful A/B persistence, and this reboot
   check are mandatory: until they pass, the image is not a normal-run
   candidate and must not enter MIT or trajectory acceptance.
4. With no motor command, wait at least 100 ms. Confirm the power stage stays
   idle; no automatic transition to `MOTOR_MODE` occurs. The PC13 status LED
   should change state at approximately 1 Hz while the controller is idle.
5. Send one explicit MIT `MOTOR_CMD` frame followed by bounded zero-reference
   frames. Confirm the motor only enables after the command and the non-blocking
   10 ms DRV precharge/readback gate, and that the controller replies on its
   configured CAN ID.
6. Stop sending frames. Confirm CAN timeout disables the gate driver and
   returns to the menu state. A new explicit motor command must be required to
   re-arm.
7. Repeat the command with encoder or ADC validity intentionally absent only
   if the bench can do so safely. Confirm the gate remains disabled and the
   mode request is rejected.

Do not increase duty, current, speed, or calibration limits until phase order,
current polarity/scaling, PWM polarity/deadtime, and the timeout behavior have
been recorded from the bench.
