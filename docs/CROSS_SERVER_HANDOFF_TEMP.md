# Athena 跨服务器临时交接文档

> 用途：这是 31.20 重启后恢复工作的临时交接文件。它把当前对话中最重要的
> 固件来源、实机状态、刷写边界和后续操作集中记录，供另一台服务器上的 Codex
> 接续使用。后续对话和工程状态恢复正常后删除本文件。

## 当前唯一开发主线

- 本地与远端工程：`athena_motorcontrol`
- 唯一开发分支：`cyberdog-safe-bringup`
- 当前主线提交：`27dad4d0b35c0580a9724aa89a7a7f80aa8c0690`
- 正式镜像来源提交：`197f5cde318640f3332ed525ea29b96cab55c478`
- 正式构建工具链：项目内 `.toolchains/arm-gnu-15.3/bin`
- 正式构建命令：
  `GCC_PATH=/Users/choqy/workspace/xiaomi_dog/.toolchains/arm-gnu-15.3/bin make release-normal`

不得从 `experiment/current-loop-pi-measurement` 生成新的固件。历史 v8 实验目录
只作为证据保留在 `athena_motorcontrol/tmp/validation/`。

## 当前正式镜像

- 路径：`athena_motorcontrol/artifacts/athena_mainline_release_197f5cd_20260905/motorcontrol.bin`
- SHA-256：`e640fd1f1fa5bdee1a86b3c4d6b9a9d1ef4ac64832d54b3318aa42f2ee53c077`
- Provenance：同目录 `build.provenance.txt`
- WebUI 选择：`athena_motorcontrol/athena_bench_webui.json`
- 该 BIN 与历史 v8 BIN 字节级一致；区别在于现在由主线提交和正式 provenance 管理，
  不再把实验分支作为固件来源。

## 已验证固件能力

- TIMER0 UPDATE 同步 ADC 注入采样。
- 内部 d/q 电流 PI 闭环已完成可用性验证；默认实验增益为 RAM-only 结果，未擅自
  改成新的生产默认值。
- MIT 五参数以电机侧 P/V/Kp/Kd/力矩语义工作；WebUI 只在输出端输入时按 9:1
  映射为电机侧命令。
- `P_MIN/P_MAX=-100/100`、`V_MIN/V_MAX=-65/65`、`CAN_TIMEOUT=3000` 的 v8
  运行条件有历史实机证据。
- 位置 MIT 空载候选：`Kp=10`、`Kd=0.5`、重力补偿 `0`、摩擦幅值 `0.3 Nm`；
  仅作为候选起点，不是负载下最终参数。

## 31.20 当前状态

- SSH 主机：`192.168.31.20`（`MacBookAir.lan`）
- WebUI（本次交接后应使用）：`~/workspace/xiaomi_dog/athena_motorcontrol/tools/athena_bench_webui.py`，端口 `8788`
- 旧运行目录：`~/athena_runtime/`，仅作为迁移前备份，不再作为固件来源。
- 本次重启后已恢复服务，UC12 桥接为 `/dev/ttys001`，1 Mbit/s。
- 已完成只读 `diag-ping`、`diag-snapshot`、`diag-drv-status`；CAN 响应正常。
- 只读页观察：`state=0`、`runtime_gate_flags=0x0000000C`、`runtime_i_max=40.000 A`。
- 当前未使能、未运动、未执行刷写；新工程已同步到 `~/workspace/xiaomi_dog/`，远端 Git 工作区应保持干净。
- 重启后旧 WebUI 曾指向 `83e8828b...` 镜像；本次交接后应切换为本主线正式镜像，
  不得把旧 SHA 当成主线验证结果。

## 远端继续工作的顺序

1. 在 `~/workspace/xiaomi_dog` 中确认 `git -C athena_motorcontrol status`、分支和
   `docs/FIRMWARE_VERSION_MANIFEST.md`。
2. 确认 UC12 桥接由当前 WebUI 独占；先做只读诊断并记录板 UID
   `39305137-14303434-47457A29`。
3. 如需刷写，先对同一 UID 做双份完整 Flash 备份和 preflight；核对
   `source commit + provenance + BIN SHA + board UID` 四元组。
4. 使用主线正式镜像执行 hash-locked `flash-normal`，读回 SHA 必须一致，再
   `boot-normal`。
5. 每次正常固件刷写后必须重新校准，并记录 ordering/calibration、PPAIRS、E_ZERO、
   LUT checksum、配置 CRC；重启后再次读回确认，之后才能做 MIT/轨迹测试。
6. 实机验证应分别记录：服务连通、固件已刷写、固件已启动、校准通过、实际运动通过。
   任何一步未完成都不能宣称主线运行与 v8 一致。

## 临时文件清理

`CROSS_SERVER_HANDOFF_TEMP.md` 只用于本次跨服务器接续。待远端 Codex 能直接读取
本工程、主线和版本清单后，删除本文件并在提交记录中说明已完成交接。
