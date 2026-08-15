# GD32F303 first-flash and passive-diagnostic runbook

This runbook is only for the reviewed `SAFE_DIAGNOSTIC` release. It does not
authorize or describe energized PWM, calibration, MIT control, mass erase,
unprotect, or Option Bytes programming.

## Fixed inputs

- Image: `../artifacts/athena_safe_diagnostic_ccf6522/motorcontrol.bin`
- Base address: `0x08000000`
- Size: 29,380 bytes
- SHA-256: `9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82`
- Main-Flash erase range: `0x08000000..0x080077FF` (15 aligned 2 KiB pages)
- Reserved configuration range: `0x0803C000..0x0803CFFF`
- Expected Option Bytes SHA-256:
  `c0b942fbb9fe967ec0e7b675e080d48c930fc5fe3fde70f6dd6f9646fdffc0d3`

The fixed release artifact and factory backup live outside the Git repository
but inside the shared workspace. `tools/athena_safe_flash.sh self-test` checks
their existence, exact sizes and hashes before any hardware operation.

## Bench preparation

1. Remove the joint linkage. Secure the controller/motor housing and leave the
   output shaft free to move.
2. Connect ST-LINK SWDIO, SWCLK, GND, target-reference voltage, and NRST when
   available. Make or change wiring only while controller power is off.
3. Use the same controller power path as the verified factory backup. Set the
   12 V bench supply current limit to approximately 0.3 A for passive testing.
4. Disconnect or stop SavvyCAN, the factory WebUI service, and every other UC12
   program. Keep the supply switch/emergency stop reachable.
5. Do not continue if the motor moves, develops holding torque, makes an
   abnormal sound, heats, or reaches the supply current limit.

## Read-only preflight and current-state backup

Run offline checks first:

```sh
make host-test host-tools-test
tools/athena_safe_flash.sh self-test
```

With ST-LINK connected and the controller powered:

```sh
tools/athena_safe_flash.sh preflight
tools/athena_safe_flash.sh backup
```

The hardware preflight requires:

- target voltage between 3.0 and 3.4 V;
- debug/device word `0x17010414`;
- Cortex-M4 SWD DAP ID `0x2BA01477` (explicitly supplied to OpenOCD's
  `stm32f1x` Flash-driver target template);
- 512 KiB Flash-size word `0x0200`;
- Option Bytes identical to the verified unprotected baseline.

`backup` creates a new timestamped directory under the workspace `backups/`
folder. It reads the complete 512 KiB main Flash twice and refuses to continue
unless both reads are byte-identical. It also saves logs and SHA-256 values.

## Program while keeping the CPU halted

The write action requires both a literal acknowledgement and the complete
reviewed image hash:

```sh
tools/athena_safe_flash.sh flash-safe \
  --confirm-safe-sha 9824e0587281bd6bcf6b1915c764c46d30a1843b24d4ee488ea1b308513c1e82 \
  --i-understand-this-writes-main-flash
```

The tool performs another preflight and current-state backup, explicitly
erases only the 15 application pages, programs and verifies the image, reads
the exact image back, compares the 4 KiB reserved configuration range before
and after, and rechecks Option Bytes. The CPU remains halted at completion.

If any comparison fails, leave the target halted, switch off controller power,
and inspect the retained logs. Do not attempt a blind second write.

## Separate safe boot

Before starting the new image, reconfirm that the motor is unloaded/fixed and
the 12 V supply is current limited. Then run:

```sh
tools/athena_safe_flash.sh boot-safe
```

This action first reads the application and Option Bytes again. It only resets
and runs when the target contains the reviewed safe image exactly.

Immediately after boot:

1. Confirm no motion, holding torque, abnormal sound, heating, or current-limit
   event.
2. Measure PA11 to GND; it must remain low.
3. Do not interpret a multimeter reading on PA8/PA9/PA10 as proof that PWM is
   absent. That proof remains blocked until a scope is available.

## UC12 passive diagnostics

Build the restricted client and confirm firmware identity:

```sh
make -C tools/athena_diag_uc12 test
tools/athena_diag_uc12/athena_diag_uc12 ping
tools/athena_diag_uc12/athena_diag_uc12 snapshot
```

The client refuses continued snapshot/watch/export operation unless snapshot
page 3 reports `SAFE=1` and PA11, TIMER0 primary output, and all three PWM
channel-enable bits are zero.

For the ten-minute capture:

```sh
tools/athena_diag_uc12/athena_diag_uc12 export \
  --seconds 600 \
  --csv /absolute/path/passive_test.csv \
  --supply-volts 12.0 \
  --current-limit-amps 0.3
```

During capture, slowly turn the unloaded output shaft through several encoder
wraps, record the actual supply voltage/current separately, and gently warm one
temperature-sensor area without exceeding normal touch-safe temperature.

The passive test passes only when:

- safe output bits remain clear for the entire capture;
- encoder counts are continuous and parity/EF/jump counters do not grow;
- ADC/SPI timeout counters do not grow;
- phase-current offsets, bus ADC, temperatures, and six Hall channels are
  stable/plausible and respond to the intended manual stimulus;
- CAN remains responsive for the full ten minutes;
- the motor never produces torque.

Record the exact backup path, CSV path, supply conditions, observations, and
pass/fail result in `AGENT.md`.

## Recovery boundary

The tool contains a hash-locked `restore-factory` action so recovery is
available, but using it requires a separate explicit authorization. It restores
and verifies the complete main Flash, leaves the CPU halted, and never writes
Option Bytes. Option Bytes should only be restored through a separately
reviewed device-specific procedure if a read proves they actually changed.

No PWM or motor-motion test follows this runbook. A scope measurement of PA11
and PA8/PA9/PA10 is the next hard gate.
