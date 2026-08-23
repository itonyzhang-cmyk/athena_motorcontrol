Normal recovery image built 2026-08-23.

Changes:
- A new 0xFC enable frame starts a fresh CAN watchdog session, so recovery
  after timeout does not require a board reset.
- An explicit 0xFC re-arms only the latched gate-driver fault bit; DRV
  nFAULT/readback, encoder and ADC preflight checks still gate PWM output.
- The UC12 SLCAN bridge uses bounded non-blocking PTY writes; an unread slave
  cannot stall the USB CAN receive thread.

Image: motorcontrol.bin
SHA-256: 229ddb217655131f9a3ab577932e5061e30d18fef41325805d1b68a4c43bf55c
Size: 55244 bytes
