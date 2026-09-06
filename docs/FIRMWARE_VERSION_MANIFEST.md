# Firmware Version Manifest

## Mainline release

主线代码基线（已验证代码）为 `cyberdog-safe-bringup@b6ba088996150f0285d2e499e0440ee8d860d091`。
其后的提交仅修正 provenance/交接文档，不改变固件源码或正式 BIN；因此不作为新的固件版本。

| Version | Source | Image SHA-256 | Role | Hardware status |
| --- | --- | --- | --- | --- |
| `mainline-197f5cd` | `cyberdog-safe-bringup@197f5cde318640f3332ed525ea29b96cab55c478` | `e640fd1f1fa5bdee1a86b3c4d6b9a9d1ef4ac64832d54b3318aa42f2ee53c077` | Current normal firmware; promoted v8 current-loop/MIT changes plus mainline WebUI tracking | 31.20 已完成 hash-locked 刷写、读回、启动、校准和 MIT 位置/速度回归 |

### 2026-09-05 主线与 v8 远端回归

- 详细报告：`tmp/validation/artifacts/athena_mainline_remote_regression_20260905/RESULTS.md`
- 本轮回归实际测试提交：`b6ba088996150f0285d2e499e0440ee8d860d091`
- 回归报告原始记录提交：`a4d9df0571cc612629a453f46ec1c28267d8d58b`（仅报告记录）
- 实际刷写/测试工具提交：`00837104b4dc2ab6c97e5283b3d3bed4af81fe31`
- 该提交还修复了远端 OpenOCD 必须显式传入脚本目录的问题；不改变正式 BIN 内容。
- 结果：主线 BIN 与 v8 BIN 字节级一致，目标板实际运动方向、停止帧、校准门禁和
  低速摩擦受限行为与 v8 证据一致。

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
