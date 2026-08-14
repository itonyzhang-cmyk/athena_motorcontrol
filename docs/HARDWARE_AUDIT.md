# Xiaomi Controller Hardware Cross-Audit

Date: 2026-08-14

Compared sources:

- `athena_motorcontrol` commit `4c443e6`
- `fanmyu/dgm-xiaomi` master as inspected on 2026-08-14
- existing factory-firmware analysis and controller measurements

This audit establishes code-level compatibility only. It is not evidence that
the current image is safe to run on the power stage.

## Summary Matrix

| Subsystem | Athena | DGM-Xiaomi | Result |
|---|---|---|---|
| MCU | GD32F303 HD, Cortex-M4F | Same | Match |
| Memory | 512 KiB Flash, 64 KiB SRAM | Same Keil target | Match; fix copied linker header |
| System clock | 8 MHz HXTAL to 120 MHz | Same | Match; verify crystal/timing on board |
| PWM pins | PA8/PA9/PA10, TIMER0 CH0/1/2 | Same | Physical match |
| PWM timing | center-aligned, about 30 kHz | center-aligned, 20 kHz | Intentional difference; audit sampling/DT |
| DRV8323 SPI | PB12 CS, PB13/14/15 SPI1 | Same | Match |
| DRV EN/nFAULT | PA11/PA12 | Same | Match; Athena shutdown logic unsafe |
| Encoder SPI | PA15 CS, PC10/11/12 SPI2 remap | Same | Match |
| Phase current | PC0 channel10, PC1 channel11 | Same physical pins | Logical phase/order conflict; must measure |
| Bus voltage | PC3 channel13 | Same | Match; scale differs slightly |
| PCB/motor temperature | PB0 ch8, PC2 ch12 | Not used by DGM | Needs factory/measurement validation |
| Six analog Hall | PA4-7, PC4-5 | Same physical channels | Logical ordering/use unverified |
| CAN | PB8/PB9 partial remap, 1 Mbps | Same | Match; sample point differs |
| UART | PA2/PA3 USART1 | Same | Match; baud rate is firmware-defined |
| Flash config | 0x0803C000 | 0x08032000 | Different layouts; no interoperability |

## P0 Hardware Findings

### Phase and current semantics

Athena maps U/V/W to TIMER0 CH0/CH1/CH2. DGM's FOC mapping effectively maps
phase A/B/C to CH2/CH1/CH0. Athena reads PC1 as one phase and PC0 as another,
while DGM names PC0 phase A and PC1 phase B. Variable names are not sufficient
to resolve the real phase order.

Required test sequence:

1. PA11 stays low.
2. Verify PA8/PA9/PA10 timing and polarity with an oscilloscope.
3. At low voltage and with a current-limited supply, inject one bounded phase
   vector at a time.
4. Record physical terminal response and PC0/PC1 sign.

Relevant Athena locations:

- `Core/Src/gpio.c:113`
- `Core/Src/tim.c:181`
- `Core/Src/adc.c:438`
- `Core/Src/foc.c:81`

### ADC regular-channel defect

`Core/Src/adc.c:424-431` intends:

- ADC0: channels 6, 15, 7, 8
- ADC1: channels 5, 14, 4, 12

The last line configures ADC0 rank 3 to channel 12, overwriting channel 8;
ADC1 rank 3 is never configured. Current temperature fields are invalid until
this is corrected and the dual-ADC DMA packing is verified.

### DRV8323 protection

Pin/SPI mapping agrees, but Athena:

- raises PA11 before completing DRV configuration;
- does not configure/read back all gate-drive registers;
- ends with `DIS_SEN=1`, disabling the DRV sense overcurrent function;
- does not immediately shut down on nFAULT.

DGM is useful evidence for register values but its code is not to be copied.
The final configuration must be derived from the DRV8323 datasheet and verified
by readback.

### Encoder

Both implementations read a 14-bit angle using the same pins and SPI mode.
Athena's `14-bit << 2` representation with `ENC_CPR=65536` is internally
consistent. Missing items are parity, EF/ERRFL, DIAAGC/magnetic validity, SPI
timeout, frozen value and implausible jump detection.

### Scaling

- Athena current scale: `0.0201416 A/count`.
- DGM formula using 1 mOhm and CSA gain 40 gives approximately the same value.
- Athena voltage scale: `0.0128906 V/count`.
- DGM uses a nominal divider factor of 16.3, approximately
  `0.0131355 V/count`.

These are hypotheses until resistor values, DRV gain readback, ADC reference,
and meter-versus-ADC measurements agree.

### Flash

Athena writes preferences at `0x0803C000`, but the linker currently makes the
entire 512 KiB Flash available to application code. The application region must
end before the reserved configuration pages, with a linker assertion preventing
overlap. Configuration needs version/length/CRC and a power-loss-safe commit.

## Hardware Validation Order

1. MCU ID, reset cause, clocks and memory map.
2. UART and CAN without any motor-state transition.
3. DRV SPI registers while power outputs remain impossible.
4. Encoder raw/status/parity and hand-rotation continuity.
5. ADC raw/DMA mapping, voltage, current offsets, temperatures, Hall channels.
6. PWM pins on a scope with PA11 low.
7. Low-voltage/current-limited phase mapping.
8. Only then current-loop and calibration work.
