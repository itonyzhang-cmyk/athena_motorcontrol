# BRINGUP_INJECT 台架运行手册

本手册用于在 MCU 引脚没有探针或示波器的情况下，首次给电机通电测试。电机本体、编码器和相电流 ADC 通道就是测试仪器。本镜像不是经过评审的 `SAFE_DIAGNOSTIC` 镜像，也不是电机控制固件。

## 固定输入

- 镜像：`../artifacts/athena_inject_bringup_drv_spi_rate_fix_20260819/motorcontrol.bin`
- 基地址：`0x08000000`
- 大小：32,644 字节
- SHA-256：
  `4bb4c470b2fc502ce5e4fe94e0128eeed992a4bff7e7266c166bf6c27beb72ad`
- 擦除范围：`0x08000000..0x080087FF`（17 个按 2 KiB 对齐的页）
- 保留配置范围：`0x0803C000..0x0803CFFF`
- 源码基线：`30a0b2e` 加工作区 CAN/DRV 唤醒状态机、PA11 输出锁存器检查、DRV 10 ms 启动等待和 3.75 MHz SPI 修复

## 台架设置

1. 将测试电机连接到控制器。拆除所有连杆；固定电机外壳，并让输出轴保持空载、可自由转动。
2. 使用限流台式电源给控制器供电，设置为 12 V，限流 **0.2 A**。本手册执行期间任何时刻都不得超过 0.5 A。
3. 将电源开关或急停装置放在触手可及的位置。
4. 停止 SavvyCAN、WebUI 服务以及所有其他 UC12 程序。UC12 同时只能由一个客户端占用。
5. 只有刷写步骤需要 ST-LINK；如果 ST-LINK 与台架共用地线，通电测试前应将其拔下。

## 刷写与启动

`flash-inject` 会自动创建当前状态备份；只有在两次完整 Flash 读取结果一致且选项字节与记录的基线匹配时，它才会继续执行。应用区按 17 个 2 KiB 页逐页擦除、写入并校验；任一页失败即停止，CPU 保持停机。

```sh
tools/athena_safe_flash.sh self-test
tools/athena_safe_flash.sh identify
tools/athena_safe_flash.sh flash-inject \
  --confirm-inject-sha 4bb4c470b2fc502ce5e4fe94e0128eeed992a4bff7e7266c166bf6c27beb72ad \
  --i-understand-this-writes-main-flash
tools/athena_safe_flash.sh boot-inject
```

编程完成后 CPU 仍保持停机状态。`boot-inject` 会在复位前重新读取并校验完整镜像和选项字节。仅当电机已连接且电源准备就绪时才可使用它。

## 首次被动检查

编译并运行客户端：

```sh
make -C tools/athena_diag_uc12 test
tools/athena_diag_uc12/athena_diag_uc12 ping
tools/athena_diag_uc12/athena_diag_uc12 drv
tools/athena_diag_uc12/athena_diag_uc12 snapshot
```

通过标准：

- Snapshot 第 3 页：`SAFE=1`、`nFAULT=0`、`PA11=0`、`enc=1`、`adc=1`。在此配置中，`POEN=1` 和 `CH=111` 是预期且安全的，因为唯一的电源门控信号是 PA11。
- 不得有运动、保持转矩、异常声音或发热。
- 普通 `drv` 在 PA11 低时可能全部为 `0xFFFF`，这不代表配置成功；只能用下面的 `drv-wake` 做受限唤醒验证。

如果 PA11 为高电平、nFAULT 为低电平，或编码器/ADC 无效，应停止并重新检查连接。不得继续操作。

## DRV 唤醒和配置验证

若普通 `drv` 回读为 `0xFFFF`，使用以下命令。它会先禁用 TIMER0 主输出并将三路比较值设为全低，再将 PA11 拉高 10 ms（与原始 DRV 初始化的硬件就绪等待一致）；主循环在窗口结束后写入受限 DRV 配置并读取验证，最后无条件拉低 PA11。该命令成功前，`inject` 会被固件拒绝。

```sh
tools/athena_diag_uc12/athena_diag_uc12 \
  --confirm-drv-wake drv-wake
```

通过条件：命令返回 `drv_ready=1`，后续页显示 `FSR1/FSR2=0`、`DCR=0x00A0` 或 `0x00A1`（清故障位会自清零）、`CSACR=0x02DC`、`OCPCR=0x0415`。任何失败都保持 PA11 为低；不得转而尝试注入。

失败时不要重复执行该命令。一次 `drv-wake` 已锁存页面 31..48：八笔 SPI
TX/RX 与 TBE/RBNE/BUSY 完成状态、SPI1 控制/状态寄存器，以及 PB12..PB15 和
PA11/PA12 所在端口的 GPIO 配置、输出锁存和输入状态。保存这一整段输出后再判断，
避免把 MCU 传输超时、MISO 全高、或引脚复用错误混为一谈。

## 注入流程

每条命令只发出一个脉冲。工具每次都要求 `--confirm-inject`，并且必须通过预检：

```sh
tools/athena_diag_uc12/athena_diag_uc12 inject 0 0.5 10 --confirm-inject
```

以下六个向量是三路 PWM 通道上的 BLDC 步进模式：

| 向量 | CH0 (U) | CH1 (V) | CH2 (W) |
| --- | --- | --- | --- |
| 0 | 高侧占空比 | 低 | 低 |
| 1 | 高侧占空比 | 高侧占空比 | 低 |
| 2 | 低 | 高侧占空比 | 低 |
| 3 | 低 | 高侧占空比 | 高侧占空比 |
| 4 | 低 | 低 | 高侧占空比 |
| 5 | 高侧占空比 | 低 | 高侧占空比 |

先以最低设置运行完整矩阵：

```sh
for v in 0 1 2 3 4 5; do
  tools/athena_diag_uc12/athena_diag_uc12 inject "$v" 0.5 10 --confirm-inject
done
```

每个脉冲都要记录：

- 报告结果（必须为 `OK`）；
- 电源电流表现（应保持在 0.2 A 限值以内）；
- Snapshot 第 21 页 B、C 通道 ADC 的带符号峰值偏差；
- 编码器原始值是否发生变化（第 22 页）；
- 是否出现声音、运动、发热或 nFAULT 事件。

如果所有结果都正常，再对选定向量重复测试，将设置提高到 1.0% 和 20 ms，以增强响应。此阶段不得超过 5.0% / 50 ms。

任何时候都可以使用 `stop`，且它始终具有最高优先级：

```sh
tools/athena_diag_uc12/athena_diag_uc12 stop
```

## 数据解读

本流程的目的是通过真实硬件确认以下事项：

1. PA11 确实能够门控驱动器（PA11 为低时电流为零，只有在已使能的脉冲期间有电流）；
2. CH0/CH1/CH2、六个向量与电机实际端子之间的对应关系；
3. SOB/SOC（ADC0/ADC1 插入通道）相对于这些端子的映射关系和符号约定；
4. 编码器方向相对于电流向量的关系。

每个向量都应在 `peak_b`/`peak_c` 上产生可重复的带符号模式，并且在更高占空比下使编码器产生一个小幅、确定性的步进。如果结果为 `CURRENT_LIMIT` 或 `FAULT`，说明看门狗或驱动器已关闭该脉冲；应给电路板断电（这也会清除锁存故障），在调查清楚前不要重复测试。

如果所有向量都完全没有电流，最可能的原因是相线接头错误、PA11 实际未到达驱动器，或 DRV SPI 配置没有生效；请重新检查 `drv` 寄存器回读值。

## 完成本手册后的下一步

测得的向量/电流/编码器表格是修正控制固件中相序、电流采样符号和比例常数所需的证据。不得从本镜像直接开始闭环 MIT 测试；必须准备新的、经过评审的控制镜像，并获得另一份明确授权。
