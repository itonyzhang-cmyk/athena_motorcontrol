Athena normal application candidate: live CSA offset re-sampling
Target: GD32F303RET6, SAFE_BRINGUP=0, BRINGUP_INJECT=0

Change:
- After DRV enable and neutral PWM settle, discard 8 queued ADC conversions.
- Average 64 conversions with POEN and all three PWM channels enabled.
- Use that offset for the following FOC/calibration session.
- Abort the update if an ADC timeout occurs.

This artifact is built from the current source tree and has not been flashed.
BIN SHA-256: f9cfd8e4bfa1d753bee9d496b81c636b851679443b547e75f62be657fe78679c
