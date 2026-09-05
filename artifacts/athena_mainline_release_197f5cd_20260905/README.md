# Mainline normal firmware release

- Source branch: `cyberdog-safe-bringup`
- Source commit: `197f5cde318640f3332ed525ea29b96cab55c478`
- Firmware role: current mainline normal image; UPDATE-synchronized ADC/current loop,
  validated v8 MIT/configuration path, motor-side MIT units, 9:1 WebUI mapping.
- Toolchain: project-local Arm GNU Toolchain 15.3.1
- Binary SHA-256: `e640fd1f1fa5bdee1a86b3c4d6b9a9d1ef4ac64832d54b3318aa42f2ee53c077`
- Board target for prior hardware evidence: UID `39305137-14303434-47457A29`

This directory is the only normal firmware artifact selected by the checked-in
WebUI configuration. The historical v8 experiment and all other test outputs
are under `tmp/validation/` and are not valid release sources.
