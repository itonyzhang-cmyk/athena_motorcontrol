Normal application image: VBUS settling and runtime nFAULT evidence audit

SHA-256 (motorcontrol.bin):
18d4e83941d75c257799fad664cfd2b83eca7db55575dc4591dd27b73ffe2fd4

Changes:
- PC3/ADC2 VBUS uses a 55.5-cycle acquisition window; phase-current ADC timing
  is unchanged.
- Failed FSR SPI capture is reported as 0xFFFFFFFF instead of a false-clear
  FSR pair.
- Fault pages 130..136 retain current-loop and PWM evidence captured at the
  nFAULT edge.

This image does not change phase order, PPAIRS, KT/GR, current limits, or
DRV8323 protection thresholds.
