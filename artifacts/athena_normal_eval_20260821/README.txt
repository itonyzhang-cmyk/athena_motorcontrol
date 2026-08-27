Athena normal application evaluation firmware
Date: 2026-08-21
Profile: SAFE_BRINGUP=0 BRINGUP_INJECT=0
Target: GD32F303RET6, image base 0x08000000

This is an unflashed evaluation artifact. It contains the normal MIT
five-parameter control path, but does not authorize hardware programming or
motion testing. Follow docs/NORMAL_APPLICATION_RUNBOOK.md and the project
safety gates before any bench operation.

Artifacts are copied from the independently rebuilt current source tree:

motorcontrol.bin  52372 bytes
SHA-256           538da9dc8aac7c4e144de6d6b8f126fbc8f7fee348ef1976ed6b9a6e72330afa

motorcontrol.elf
SHA-256           c1aa3e9f57172b47285b86001dbf61ecbd6402a45b0cf12c70d82311ac8744b6

motorcontrol.hex
SHA-256           2a6bde0ef7caf341fdc33605c1f20686a964beec67fcace08c6802618803fd70

Offline evidence:
- make host-test host-app-test host-tools-test: PASS
- tools/verify_normal_image.sh: PASS
- no controller flash, boot, CAN motor command, or motion performed
