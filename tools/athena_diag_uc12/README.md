# ATHENA-DIAG UC12 client

Restricted read-only host client for the first-flash `SAFE_DIAGNOSTIC`
firmware. It owns the UC12 directly and only transmits valid standard CAN
requests to `0x701`. It has no arbitrary CAN, MIT, enable, calibration,
configuration, reset, or Flash-write interface.

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
