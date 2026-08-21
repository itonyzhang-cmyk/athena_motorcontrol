# ATHENA-DIAG UC12 client

Restricted host client for the `SAFE_DIAGNOSTIC` and `BRINGUP_INJECT`
firmware profiles. It owns the UC12 directly and only transmits valid standard
CAN requests to `0x701`. With the `SAFE_DIAGNOSTIC` profile it has no MIT,
enable, calibration, configuration, reset, or Flash-write interface. With the
`BRINGUP_INJECT` profile it additionally exposes the gated single-phase
injection pulse (`inject`), immediate shutdown (`stop`), and live DRV8323
register readback (`drv`).

Build and run the offline protocol checks:

```sh
make -C tools/athena_diag_uc12 test
```

SavvyCAN, the factory WebUI service, and other UC12 programs must be stopped
before using the client:

```sh
tools/athena_diag_uc12/athena_diag_uc12 ping
tools/athena_diag_uc12/athena_diag_uc12 info
tools/athena_diag_uc12/athena_diag_uc12 snapshot
tools/athena_diag_uc12/athena_diag_uc12 watch --seconds 600
tools/athena_diag_uc12/athena_diag_uc12 export --seconds 600 \
  --csv /absolute/path/passive_test.csv \
  --supply-volts 12.0 --current-limit-amps 0.3
```

Defaults are CAN channel 0, 1 Mbit/s, a 1-second response timeout, and a
25 ms minimum request interval. `watch`/`export` rotate through all implemented
raw snapshot pages and the encoder/ADC error counters. The narrow CSV format
keeps every response with its host timestamp, opcode, page, status, raw payload,
and the operator-entered supply conditions.

The most important output is snapshot page 3. A passive boot passes only when
bit 31 (`SAFE`) is set and bits 1, 2, 8, 9, and 10 (PA11, TIMER0 primary output,
and the three PWM channel enables) are all clear.

## BRINGUP_INJECT commands

The injection firmware boots passive (PA11 low, all PWM channels at the
all-low position) and only opens the gate for a single validated pulse. Duty
and duration are fixed tables that mirror the firmware limits: 0.5..5.0 %
high-side duty and 10..50 ms. The vector selects one of six BLDC step
patterns. An explicit `--confirm-inject` and a passing safety preflight are
required before the pulse is armed:

```sh
tools/athena_diag_uc12/athena_diag_uc12 inject 0 0.5 10 --confirm-inject
```

After the pulse the tool waits and prints snapshot pages 20..23 (status,
peak current ADC deviations, encoder raw start/end, active ticks, and latched
faults). `stop` immediately aborts any active pulse and returns the driver to
the passive state; `drv` prints the DRV8323 fault and configuration registers.

`drv-wake` also prints a latched transaction trace: all eight SPI TX/RX words,
their TBE/RBNE/BUSY completion state, SPI1 control/status, and GPIO A/B
configuration, output-latch, and input snapshots. This trace is read-only
after the bounded PA11 window and is intended to distinguish a peripheral
timeout from a no-response MISO line.

These commands are bench bring-up tools, not a motor API. They are only valid
against the `BRINGUP_INJECT` firmware and must never be used without the
current-limited supply, an unloaded/fixed motor, and a reachable emergency
stop.
