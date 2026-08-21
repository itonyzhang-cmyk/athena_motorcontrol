Athena normal application with CAN RX pre-emption correction
Date: 2026-08-21
Profile: SAFE_BRINGUP=0 BRINGUP_INJECT=0
Target: GD32F303RET6, image base 0x08000000

This normal application retains the MIT protocol: standard ID 0x001, DLC 8,
with feedback on standard ID 0x000, DLC 6. It additionally accepts exactly
one non-MIT compatibility request: a CRC-valid ATHENA-DIAG PING request on
standard ID 0x701, opcode 0, page 0. It replies on standard ID 0x781 with
the ATHN payload. All other ATHENA-DIAG requests, including driver wake,
injection, and motor-control operations, are ignored.

The GD32 default PRE2_SUB2 grouping now assigns nFAULT EXTI pre-emption level
0, CAN RX0 level 0 subpriority 1, and the 30 kHz TIMER0 control ISR level 1.
CAN RX therefore pre-empts the control ISR, while nFAULT remains higher than
CAN. CAN timing, remap, GPIO setup, filter policy, MIT handling, and motor
gate policy are unchanged.

motorcontrol.bin  52828 bytes
SHA-256           bac3554fa8e230fc7dae72bfb3fbef1c5e17376fd8b3ccf0011d1f4d3d373367

Offline verification passed:
- make host-test host-app-test host-tools-test
- normal image symbol audit
- no BRINGUP_INJECT path linked
- flash script artifact/range self-test
