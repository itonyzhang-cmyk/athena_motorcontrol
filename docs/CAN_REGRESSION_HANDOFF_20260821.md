# CAN 回归排查交接

请基于当前工作树排查一个固件回归：旧的正常应用镜像可以启动，PC13 LED 闪烁，并且通过 UC12 + SLCAN 收到过控制器回复；当前源码重新构建出的新镜像刷入后 CAN 无法通信。不要先猜测或大范围重构，先用二进制、源码和运行日志定位差异。

## 已知镜像

- 已知可通信评估镜像：`artifacts/athena_normal_eval_20260821/motorcontrol.bin`
  - SHA-256：`538da9dc8aac7c4e144de6d6b8f126fbc8f7fee348ef1976ed6b9a6e72330afa`
- 当前源码独立重建镜像：
  - SHA-256：`5687609beeabd8fea8a4bf4566fd9847589ee0f7e92fd562d9d314ab9388167c`
- 两个 BIN 已确认不一致。不要把旧镜像当作当前源码的产物。

## 已知硬件和总线

- MCU：GD32F303RET6
- CAN：经典 CAN，11-bit standard frame，1 Mbps
- CANH/CANL 已经实测方向正确
- UC12 + SLCAN 使用 `/dev/ttysXXX`
- 正常应用默认配置：接收 ID `0x001`，回复 ID `0x000`
- 正常应用要求：标准数据帧、DLC=8、ID=`CAN_ID`

## 已知成功现象

旧镜像运行时，发送：

```sh
printf 't00187fff7ff0000007ff\r' > /dev/ttysXXX
```

曾收到：

```text
t0006017FFF7FF7FF
```

这证明旧镜像至少完成过 UC12 -> CAN 控制器接收，以及控制器 -> UC12 回复。

## 当前桥接工具证据

桥接工具已重新编译，使用：

```sh
cd /Users/choqy/workspace/xiaomi_dog/tools/uc12_slcan_bridge
./uc12_slcan_bridge --unsafe-tx --trace
```

发送日志：

```text
TRACE CAN TX t001#7FFF7FF0000007FF
TRACE UC12 TX ACK 13 00 01
SLCAN CAN TX accepted: ID 0x001, DLC 8.
```

这只能证明 Mac -> UC12 的 USB 发送请求被接受，不代表控制器收到或回复。真正的控制器回复必须出现：

```text
TRACE CAN RX t000#...
TRACE SLCAN RX t0006...
```

## 当前源码中应审计的 CAN 配置

`Core/Src/can.c`：

- `prescaler=4`
- `BS1=10TQ`
- `BS2=4TQ`
- `SJW=1TQ`
- `auto_bus_off_recovery=ENABLE`
- `no_auto_retrans=DISABLE`
- 硬件过滤器当前为全接收，软件过滤负责精确检查

`Core/Src/gpio.c`：

- PB8：CAN RX，上拉输入
- PB9：CAN TX，AF 推挽，50 MHz
- 部分重映射已启用

`Core/Src/gd32f30x_it.c`：

- RX ISR 先调用 `can_message_receive`
- 正常路径要求 standard/data/DLC8/ID=`CAN_ID`
- 匹配后先发送 `CAN_MASTER` 回复帧，再解析 MIT 数据

## 必须先完成的对比

1. 对比旧可通信镜像 ELF/map 与当前重建 ELF/map 的 `MX_CAN0_Init`、`can_rx_init`、`can_tx_init`、`USBD_LP_CAN0_RX0_IRQHandler`、`gpio`、时钟初始化和启动文件反汇编。
2. 对比当前工作树相对 `30a0b2e` 的 diff，特别是 `Core/Src/can.c`、`Core/Src/gpio.c`、`Core/Src/main.c`、`Firmware/CMSIS/GD/GD32F30x/Source/system_gd32f30x.c`、链接脚本和 Makefile。
3. 检查当前镜像是否真的以 `GD32F30X_HD`、GD32F303 链接脚本、正确启动文件构建；不能混用 STM32F446 配置。
4. 反汇编或运行时确认 CAN 外设时钟、PB8/PB9 复用、CAN0 重映射、CAN 位时序寄存器实际值。

## 修复约束

- 保留 LED 心跳和已有安全门控，不得为了 CAN 删除安全检查。
- 不得恢复旧的错误 STM32 风格硬件过滤器。
- 不得把正常应用改回 `ATHENA-DIAG` 或 BRINGUP_INJECT 协议。
- 不得修改 Option Bytes、执行 mass erase 或改变配置保留区。
- 不要先增加任意 CAN 发送或电机运动命令。
- 若怀疑 PB9 速度、GPIO 上拉或 CAN 时序，必须用原厂实测寄存器值和 GD32 数据手册解释，并提供最小改动。

## 修复后的验收

必须提供：

```sh
make host-test host-app-test host-tools-test
make SAFE_BRINGUP=0 BRINGUP_INJECT=0 BUILD_DIR=/tmp/athena-normal-fixed \
  GCC_PATH=/tmp/arm-gnu-toolchain-15.2-root-new/bin -j4
tools/verify_normal_image.sh \
  /tmp/athena-normal-fixed/unsafe/motorcontrol.elf
shasum -a 256 /tmp/athena-normal-fixed/unsafe/motorcontrol.bin
```

硬件上只做通信验证：

```sh
./uc12_slcan_bridge --unsafe-tx --trace
printf 't00187fff7ff0000007ff\r' > /dev/ttysXXX
```

桥接终端必须同时出现 `TRACE CAN TX`、`TRACE UC12 TX ACK` 和 `TRACE CAN RX t000#...`。在此之前不要发送 `0xFC` 电机使能命令，也不要宣称运动控制可用。
