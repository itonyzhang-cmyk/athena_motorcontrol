# Firmware Version Manifest

## Mainline release

当前主线指针为 `cyberdog-safe-bringup@27dad4d0b35c0580a9724aa89a7a7f80aa8c0690`。
该提交只登记发布产物和整理验证证据；下面的正式 BIN 仍由其明确记录的源提交构建。

| Version | Source | Image SHA-256 | Role | Hardware status |
| --- | --- | --- | --- | --- |
| `mainline-197f5cd` | `cyberdog-safe-bringup@197f5cde318640f3332ed525ea29b96cab55c478` | `e640fd1f1fa5bdee1a86b3c4d6b9a9d1ef4ac64832d54b3318aa42f2ee53c077` | Current normal firmware; promoted v8 current-loop/MIT changes plus mainline WebUI tracking | Binary rebuilt and symbol-audited; remote runtime validation pending flash to the post-reboot board |

## Promoted v8 equivalence

The historical v8 image has the same binary SHA-256 as `mainline-197f5cd`.
The v8 provenance names an experimental dirty branch, so it is retained only
as evidence. The mainline artifact above is the authoritative source for all
future builds, transfers, and normal flashing.

## Historical validation dispositions

- `tmp/validation/artifacts/athena_config_commit_async_v8_20260905/`: retained
  evidence for the v8 configuration/MIT bench run; do not select directly.
- `tmp/validation/artifacts/athena_mit_v8_20260906/`: retained MIT result report;
  it records the tested candidate parameters and limits, not a universal default.
- All other directories in `tmp/validation/artifacts/` are historical test
  evidence. They are not firmware baselines and must not be flashed without a
  new explicit review and hash-locked procedure.

## Build/flash rule

Every new firmware image must be built from `cyberdog-safe-bringup`, carry a
commit and SHA-256 provenance record, pass host tests and image audit, and be
identified on the remote host before flashing. The remote 31.20 workspace is
the future execution location; its handoff document explains the transfer and
post-flash calibration gates.
