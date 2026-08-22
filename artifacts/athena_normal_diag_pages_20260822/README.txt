Athena normal application with read-only diagnostic pages

Built with:
  make SAFE_BRINGUP=0 BRINGUP_INJECT=0 BUILD_DIR=/tmp/athena-normal-diag-migrate GCC_PATH=/tmp/arm-gnu-toolchain-15.2-root-new/bin -j4

BIN size: 53684 bytes

BIN SHA-256:
  d7f7e0a261c18349da91ab884754674b0d1b09e3abdce3164e89cd4de03b6dc7

Changes:
- Normal application accepts ATHENA-DIAG read-only opcodes 0..3.
- Snapshot pages 24..30 expose DRV fault/configuration/readiness evidence.
- DRV wake and injection opcodes remain unavailable.
- MIT control path is unchanged.

Verification:
- host-test, host-app-test, host-tools-test: PASS
- normal image symbol audit: PASS
- BRINGUP_INJECT compile and image audit: PASS
