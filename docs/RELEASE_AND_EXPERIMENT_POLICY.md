# Release and Experiment Policy

## Formal firmware

Every firmware image that can be selected by WebUI or flashed through the
normal path must come from a committed release/fix commit on the active
development branch. `make release-normal` records the commit, branch, subject,
compiler, policy and binary SHA in `build.provenance.txt`; the WebUI config must
reference that exact binary and SHA.

Before flashing, verify all of the following as one tuple:

```text
source commit + branch + build provenance + artifact SHA + readback SHA
```

### Post-flash calibration gate

Every normal-firmware flash requires a new motor calibration on the flashed
image. Do not reuse `E_ZERO` or the encoder LUT from a previous image, even
though `flash-normal` preserves the configuration pages: changes to ADC, PWM,
encoder, or FOC code can change the electrical-angle interpretation.

An image is not eligible for normal operation, MIT/trajectory acceptance, or
release until all of the following are recorded for the exact artifact SHA and
board UID:

1. Calibration completes without failure and reports completed ordering and
   calibration stages.
2. The A/B configuration write succeeds.
3. `PPAIRS`, nonzero `E_ZERO`, LUT checksum, and configuration CRC are read
   back immediately after calibration.
4. After `boot-normal`, those same values remain unchanged and there is no
   safety, DRV, or ADC-timeout fault.

The calibration record must include the calibration current and diagnostics
pages 95 through 115. A completed flash readback alone proves the application
binary, not that its electrical-angle calibration is valid.

## Temporary validation

`cyberdog-safe-bringup` is the only local development trunk. All future source
changes start there and every firmware build records that trunk commit. Do not
create a second source line for experiments.

An experimental hypothesis may be built from a dirty worktree only when it is
explicitly marked `EXPERIMENTAL - DO NOT FLASH AS NORMAL`, hash-locked, and
kept out of `athena_bench_webui.json`. Once it has passed the defined hardware
checks, incorporate the exact source change into `cyberdog-safe-bringup`, run
the full regression suite, and commit it before using it as a later baseline.
If it fails, discard the uncommitted experiment rather than retaining an
alternative firmware source.

Generated build directories, Python caches, and bench artifacts are ignored by
default. A formal artifact may be force-added only together with its provenance
record, source commit, compiler identity, and SHA-256; an untracked binary is
never a selectable firmware baseline.

## Review and disposition

At the next suitable review point, every experiment must be assigned one
disposition:

1. **Promote**: retain the change in a new formal fix/release commit, with
   host tests, image verification and hardware evidence.
2. **Revert**: remove the experiment from the release branch and record why.
3. **Discard**: remove the uncommitted experimental source and its temporary
   build output; retain only a concise evidence record if it is useful.

No experiment is complete while its disposition is undecided. A regression
review must compare the last motion-proven image, not merely the latest clean
commit.
