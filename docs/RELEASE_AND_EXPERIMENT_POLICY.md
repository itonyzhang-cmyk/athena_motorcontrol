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

## Temporary validation

Temporary experiments must use a separate local branch or an explicitly named
experimental commit, for example `experiment/velocity-window-128`. They must
not update `athena_bench_webui.json` or the normal release artifact. Build them
with `ALLOW_EXPERIMENTAL_RELEASE=1` only when the hardware test itself requires
it, and label the artifact `EXPERIMENTAL - DO NOT FLASH AS NORMAL`.

The commit message must state the experiment and its decision boundary. Do not
describe an experiment as a fix until motion evidence supports it.

## Review and disposition

At the next suitable review point, every experimental commit must be assigned
one disposition:

1. **Promote**: retain the change in a new formal fix/release commit, with
   host tests, image verification and hardware evidence.
2. **Revert**: remove the experiment from the release branch and record why.
3. **Retain separately**: keep it only on the experiment branch with a clear
   README and no normal-image reference.

No experiment is complete while its disposition is undecided. A regression
review must compare the last motion-proven image, not merely the latest clean
commit.
