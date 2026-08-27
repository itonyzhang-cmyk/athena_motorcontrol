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
   torque, or a current-limit event. At this point `athena_diag_uc12` is not a
   valid application protocol client because the normal image consumes CAN
   frames as MIT control traffic.
   An uninitialized/legacy configuration page is expected to select the RAM
   defaults: receive ID `0x001`, reply ID `0x000`, and timeout 1000 cycles.
3. With no motor command, wait at least 100 ms. Confirm the power stage stays
   idle; no automatic transition to `MOTOR_MODE` occurs. The PC13 status LED
   should change state at approximately 1 Hz while the controller is idle.
4. Send one explicit MIT `MOTOR_CMD` frame followed by bounded zero-reference
   frames. Confirm the motor only enables after the command and the non-blocking
   10 ms DRV precharge/readback gate, and that the controller replies on its
   configured CAN ID.
5. Stop sending frames. Confirm CAN timeout disables the gate driver and
   returns to the menu state. A new explicit motor command must be required to
   re-arm.
6. Repeat the command with encoder or ADC validity intentionally absent only
   if the bench can do so safely. Confirm the gate remains disabled and the
   mode request is rejected.

Do not increase duty, current, speed, or calibration limits until phase order,
current polarity/scaling, PWM polarity/deadtime, and the timeout behavior have
been recorded from the bench.
