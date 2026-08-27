# SAFE_DIAGNOSTIC Interface

This document describes the first-flash, read-only diagnostic profile built
with the default `SAFE_BRINGUP=1` setting. It is deliberately separate from
the MIT motor-control protocol and from Xiaomi factory CAN extensions.

## Safety properties

- DRV8323 `EN_GATE` (PA11) remains low.
- TIMER0 CH0/CH1/CH2 outputs and the primary-output enable remain disabled;
  compare values are repeatedly forced to zero.
- CAN and UART cannot select motor, calibration, setup, zero, or Flash-write
  paths. Those symbols are absent from the linked safe image.
- UART is transmit-only. Received bytes cannot reach the legacy CLI.
- Encoder SPI, ADC conversion, and UART transmission waits are bounded.
- No Flash erase/program function is linked into the safe image. The existing
  configuration pages at `0x0803C000..0x0803CFFF` remain reserved and unchanged.

## UART

- USART1 on PA2 TX, 115200 bit/s, 8N1.
- PA3 RX is disabled in the safe profile.
- The boot banner contains `SAFE_DIAGNOSTIC`.
- Once per second the firmware emits two lines:
  - `S1`: uptime, latched faults, gate/PWM state, AS5047 raw diagnostics, and
    CAN error register.
  - `A1`: raw phase-current, bus-voltage, temperature, and six Hall ADC values.

Raw values are intentional. Current, voltage, and temperature scaling has not
yet been validated on the physical board and is therefore not claimed by this
image.

## CAN

- Classic CAN 2.0, 1 Mbit/s.
- 11-bit standard DATA frames only, DLC exactly 8.
- Request ID: `0x701`.
- Response ID: `0x781`.
- Invalid ID/IDE/RTR/DLC/magic/version/reserved/CRC requests are silently
  dropped and counted.
- Responses are limited to 50 per second. Automatic retransmission is disabled
  in this profile.

Request bytes:

| Byte | Meaning |
| --- | --- |
| 0 | `0xA5` |
| 1 | `0x5A` |
| 2 | protocol major `0x01` |
| 3 | opcode |
| 4 | sequence |
| 5 | page |
| 6 | reserved, must be zero |
| 7 | CRC-8/ATM over bytes 0..6, polynomial `0x07`, initial value zero |

Response bytes:

| Byte | Meaning |
| --- | --- |
| 0 | request opcode OR `0x80` |
| 1 | sequence echo |
| 2 | page echo |
| 3 | status: 0 OK, 1 bad page, 2 busy, 3 unavailable, 4 unsupported |
| 4..7 | unsigned 32-bit payload, little-endian |

Read-only opcodes:

- `0x00 PING`
- `0x01 GET_INFO`
- `0x02 GET_SNAPSHOT`
- `0x03 GET_COUNTER`

There are no enable, stop, zero, calibration, configuration, reset, clear, or
Flash commands in this profile.

Golden request vectors:

```text
PING seq=1 page=0       A5 5A 01 00 01 00 00 7D
INFO seq=2 page=0       A5 5A 01 01 02 00 00 D6
SNAP seq=3 page=3       A5 5A 01 02 03 03 00 B8
COUNTER seq=4 page=0    A5 5A 01 03 04 00 00 87
```

Snapshot page 3 is the most important safety word. Bits 0..10 indicate
nFAULT active, PA11 high, TIMER0 primary output enabled, encoder valid, ADC
valid, CAN warning/passive/bus-off, and CH0/CH1/CH2 enabled. Bit 31 identifies
the safe profile. A passive first boot is acceptable only when PA11, primary
output, and all three channel-enable bits remain zero.

## Build-time verification

```sh
make host-test
make BUILD_DIR=/tmp/athena-safe \
  GCC_PATH=/Users/choqy/toolchains/arm-gnu-toolchain-15.2/bin -j4
make BUILD_DIR=/tmp/athena-safe \
  GCC_PATH=/Users/choqy/toolchains/arm-gnu-toolchain-15.2/bin verify-safe
```

`verify-safe` rejects images containing known Flash-write, gate-enable, FOC,
calibration, legacy FSM, or MIT command-unpack symbols, and verifies that the
image does not overlap the reserved configuration pages.
