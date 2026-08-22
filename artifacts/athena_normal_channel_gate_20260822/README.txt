Athena normal application with read-only diagnostics and PWM channel gating.

Built with:
  make SAFE_BRINGUP=0 BRINGUP_INJECT=0 BUILD_DIR=/tmp/athena-normal-channel-gate GCC_PATH=/tmp/arm-gnu-toolchain-15.2-root-new/bin -j4

BIN size: 53820 bytes
BIN SHA-256:
  7b4539af52a3d845d31b3488405a3f594ac14ddde23830b89f4604a020dc13fc

Change relative to athena_normal_diag_pages_20260822:
- TIMER0 CH0/CH1/CH2 remain disabled at normal-image boot and every stop/fault.
- The three channel bits are enabled only after the normal DRV enable/readback
  transaction succeeds; POEN is then enabled last.
- Read-only ATHENA-DIAG pages and MIT control framing are unchanged.

Verification:
- host-test, host-app-test, host-tools-test: PASS
- normal image symbol audit: PASS
- BRINGUP_INJECT compile and image audit: PASS
