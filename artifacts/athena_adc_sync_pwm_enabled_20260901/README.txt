EXPERIMENTAL ONLY - not a release image.

Purpose: verify that TIMER0 CH3 emits recurring injected-ADC triggers when
configured as an internal PWM compare edge. The prior CH3 timing-mode image
produced one sample and subsequently lost ADC validity.

Source branch: experiment/current-loop-pi-measurement
Build inputs: SAFE_BRINGUP=0 BRINGUP_INJECT=0 ADC_SYNC_TRIGGER=1
             ALLOW_EXPERIMENTAL_RELEASE=1
Toolchain: /Users/choqy/workspace/xiaomi_dog/.toolchains/arm-gnu-15.3/bin
Binary: motorcontrol.bin
SHA-256: 24e2a7353f1470c69e365cbd88a10a21c0f3c9306439bb738f58de7d4e7ab31e
Size: 67668 bytes

Acceptance is limited to recurring ADC sample_count with adc_valid=1 followed
by a repeatable low-amplitude d-axis current step. Restore the hash-locked
software-triggered baseline if either condition fails.
