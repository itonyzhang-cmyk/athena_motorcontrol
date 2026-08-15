# BRINGUP_INJECT bench runbook

This runbook covers the first powered motor test without a probe/scope on the
MCU pins. The motor itself, its encoder, and the phase-current ADC channels are
the instruments. This is not the reviewed `SAFE_DIAGNOSTIC` image and it is not
motor control.

## Fixed inputs

- Image: `../artifacts/athena_inject_bringup_89b6629/motorcontrol.bin`
- Base address: `0x08000000`
- Size: 31,940 bytes
- SHA-256:
  `98cac3d5bb76601b82214e349a5dba63b41218910b3a052168f207037d9f6de2`
- Erase range: `0x08000000..0x08007FFF` (16 aligned 2 KiB pages)
- Reserved configuration range: `0x0803C000..0x0803CFFF`
- Source commit: `89b6629`

## Bench setup

1. Connect the test motor to the controller. Remove any linkage; secure the
   motor housing and leave the output shaft free.
2. Power the controller from the current-limited bench supply set to 12 V and
   **0.2 A** limit. Do not exceed 0.5 A at any point in this runbook.
3. Keep the supply switch or an emergency stop within reach.
4. Stop SavvyCAN, the WebUI service, and every other UC12 program. Only one
   client may own the UC12.
5. ST-LINK is only needed for the flash steps; unplug it before powered tests
   if it shares the bench ground path.

## Flash and boot

The current-state backup is taken automatically by `flash-inject`; it refuses
to continue unless both full-Flash reads are identical and the Option Bytes
match the recorded baseline.

```sh
tools/athena_safe_flash.sh self-test
tools/athena_safe_flash.sh identify
tools/athena_safe_flash.sh flash-inject \
  --confirm-inject-sha 98cac3d5bb76601b82214e349a5dba63b41218910b3a052168f207037d9f6de2 \
  --i-understand-this-writes-main-flash
tools/athena_safe_flash.sh boot-inject
```

The CPU remains halted after programming. `boot-inject` re-reads the exact
image and Option Bytes before resetting. Use it only when the motor is
connected and the supply is ready.

## Passive first check

Build and run the client:

```sh
make -C tools/athena_diag_uc12 test
tools/athena_diag_uc12/athena_diag_uc12 ping
tools/athena_diag_uc12/athena_diag_uc12 drv
tools/athena_diag_uc12/athena_diag_uc12 snapshot
```

Pass criteria:

- Snapshot page 3: `SAFE=1`, `nFAULT=0`, `PA11=0`, `enc=1`, `adc=1`. In this
  profile `POEN=1` and `CH=111` are expected and safe because PA11 is the only
  power gate.
- No motion, no holding torque, no abnormal sound, no heating.
- DRV registers are readable and plausible. Approximate expected values
  (readback of self-clearing bits may be lower):
  - `DCR` around `0x0120` (3x PWM, OTW reported, COAST=0)
  - `CSACR` around `0x02E0` (gain 40, sense OCP enabled)
  - `OCPCR` around `0x0415` (VDS latch, 4 us deglitch, 0.45 V level)
  - `FSR1`/`FSR2` clear (`0`)

If PA11 is high, nFAULT is low, or the encoder/ADC are invalid, stop and
re-check the connection. Do not continue.

## Injection procedure

One pulse per command. The tool requires `--confirm-inject` and a passing
preflight every time:

```sh
tools/athena_diag_uc12/athena_diag_uc12 inject 0 0.5 10 --confirm-inject
```

The six vectors are the BLDC step patterns on the three PWM channels:

| Vector | CH0 (U) | CH1 (V) | CH2 (W) |
| --- | --- | --- | --- |
| 0 | high-side duty | low | low |
| 1 | high-side duty | high-side duty | low |
| 2 | low | high-side duty | low |
| 3 | low | high-side duty | high-side duty |
| 4 | low | low | high-side duty |
| 5 | high-side duty | low | high-side duty |

Run the matrix at the minimum settings first:

```sh
for v in 0 1 2 3 4 5; do
  tools/athena_diag_uc12/athena_diag_uc12 inject "$v" 0.5 10 --confirm-inject
done
```

For every pulse record:

- the reported result (must be `OK`);
- supply current behavior (should stay at the 0.2 A limit or below);
- signed peak ADC deviation on B and C from snapshot page 21;
- whether the encoder raw value moved (page 22);
- any sound, motion, heating, or nFAULT event.

Then repeat selected vectors at 1.0 % and 20 ms to strengthen the response if
everything stayed clean. Do not go above 5.0 % / 50 ms in this phase.

`stop` is available at any time and always wins:

```sh
tools/athena_diag_uc12/athena_diag_uc12 stop
```

## Interpreting the data

The purpose is to establish, from real hardware:

1. PA11 really gates the driver (zero current with PA11 low, current only
   during the armed pulse);
2. the mapping between CH0/CH1/CH2, the six vectors, and the physical motor
   terminals;
3. the mapping and sign convention of SOB/SOC (ADC0/ADC1 inserted channels)
   relative to those terminals;
4. the encoder orientation relative to the current vectors.

Each vector should produce a reproducible signed pattern on `peak_b`/`peak_c`
and, at higher duty, a small deterministic encoder step. A result of
`CURRENT_LIMIT` or `FAULT` means the watchdog or the driver shut the pulse
down; power-cycle the board (which also clears latched faults) and investigate
before repeating.

If the motor produces no current at all on any vector, the most likely causes
are a wrong phase connector, PA11 not actually reaching the driver, or the DRV
SPI configuration not taking effect; re-check `drv` register readback.

## Next step after this runbook

The measured vector/current/encoder table is the evidence needed to fix the
phase order, current-sense sign, and scale constants in the control firmware.
No closed-loop MIT test starts from this image; a new reviewed control image
and another explicit authorization are required.
