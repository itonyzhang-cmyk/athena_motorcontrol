# Xiaomi CyberDog 电机控制器 Bring-up 已验证事实与证据记录

## 1. 文档目的

本文档用于记录 `athena_motorcontrol` 项目截至 2026-08-20/21 在实际控制板上已经完成验证的内容，重点不是描述“准备怎么验证”，而是把已经通过真实硬件、CAN、SPI、编码器、ADC、DRV8323 以及安全状态机测试的事实固定下来，作为后续正常固件开发、代码审查和问题定位的基准。

本文档尤其强调“已验证”和“尚未验证”的边界。后续如果正常固件出现问题，应优先检查是否遗漏或破坏了本文档中已经证明有效的底层实现，而不是重新猜测硬件是否支持这些能力。

项目对象为小米 CyberDog 一代关节电机控制板，核心硬件链路包括 GD32F303、DRV8323RS、AS5047P 绝对磁编码器、ADC 相电流采样以及 CAN/UC12 通信。

---

## 2. 最终验证状态概览

截至本次记录，以下内容已经有实际硬件证据：

| 项目 | 状态 | 结论 |
|---|---|---|
| MCU 启动与主循环 | 已验证 | 固件能够正常运行，LED 心跳和 CAN 响应可作为运行证据 |
| CAN 物理链路 | 已验证 | MCU → CAN → UC12、UC12 → CAN → MCU 均已验证 |
| UC12 主机确认解析 | 已验证 | 早期误判来自主机工具 ACK 判断，不是 CAN 硬件故障 |
| CAN 1 Mbps 时序 | 已验证 | factory timing 可稳定工作 |
| CAN 接收过滤 | 已验证 | 硬件全接收 + ISR 软件严格过滤是已验证方案 |
| 编码器 AS5047P | 已验证 | SPI2、有效帧、编码器计数、诊断数据均可正常读取 |
| ADC | 已验证 | ADC 数据可读取，`adc=1`，超时计数为 0 |
| DRV8323 ENABLE 唤醒 | 已验证 | PA11 唤醒时序可工作 |
| DRV8323 SPI | 已验证 | 8 次 SPI 事务均完成并读回有效寄存器 |
| DRV8323 配置 | 已验证 | DCR/CSACR/OCPCR 可写入并回读 |
| DRV8323 FSR | 已验证 | 正常唤醒时 FSR1/FSR2 均为 0 |
| 无 ST-LINK 冷启动 | 已验证 | 断开 ST-LINK 后重新上电，MCU/CAN/编码器/DRV 唤醒仍可工作 |
| 安全输出关闭 | 已验证 | PA11 低、PWM 主输出关闭、故障可锁存 |
| nFAULT 安全处理 | 已验证 | 真正的 gate-driver fault 能触发安全关闭 |
| 受控注入安全门 | 已验证 | 未满足 `drv_ready`、编码器、ADC、nFAULT 等条件时禁止注入 |
| 低占空比实际 PWM/电机运动 | 尚未形成成功闭环 | 首次注入曾触发 DRV CPUV，随后已经定位并修改时序，但修复后的最终物理注入结果尚未形成新的成功证据 |
| 正常 MIT 应用 CAN → MOTOR_MODE | 尚未完成最终实机闭环 | 软件路径已经修正并完成离线审计，但最新正常镜像尚未完成最终硬件运动验证 |
| 电机实际连续旋转 | 尚未验证 | 不能把诊断注入成功或 DRV SPI 成功等同于电机运行成功 |

因此当前最准确的状态是：底层硬件访问和安全 bring-up 已经基本打通，真正尚未完成的是“正常应用固件收到 MIT CAN 命令后安全进入 MOTOR_MODE，并产生正确相序/电流/PWM，最终让电机稳定运动”的闭环。

---

## 3. CAN 通信：已经完成的硬件验证

### 3.1 早期 CAN “不通”并非硬件故障

2026-08-18 的测试首先恢复了板子的 factory firmware，并完成了整片 Flash 备份和校验。

factory firmware 的整片备份 SHA-256：

`302f25ed7848ec22c77dbce177c79f548b6de502be71f06976034f8df9cb1ec7`

随后通过 CAN TX beacon 验证了 MCU → UC12 的发送链路，证明 MCU CAN 外设、CAN TX mailbox、收发器以及 UC12 接收路径均能工作。

真正的问题出在 `athena_diag_uc12` 主机工具：它错误地把“命令响应 ACK 的 `response[2] & 0x80` 条件”套到了 CAN transmit confirmation 上，而已经验证工作的 `uc12_discover` 实现只要求 `confirmation[2] >= 1`。

因此早期出现的：

`UC12 did not confirm the diagnostic request`

不能解释成“CAN 硬件不通”。

这个问题已经修复，并且后续诊断固件可以稳定执行 `ping`、`snapshot`、`drv-wake` 等 CAN 命令。

### 3.2 CAN 参数

已验证的 CAN 配置采用 factory timing：

- APB1：60 MHz
- Prescaler：4
- BS1：10 TQ
- BS2：4 TQ
- 速率：1 Mbps
- 自动 bus-off recovery：开启
- 自动重发：开启

CAN TX 使用 PB9，已经核对为 AF push-pull 50 MHz。factory firmware 的实时 GPIO 对比也用于排除了 PB9/PB10/PB12 等 GPIO 模式差异作为 CAN 根因。

### 3.3 主机工具的限速问题

诊断固件的只读请求存在约 20 ms 的响应限速。连续快速轮询时偶尔会出现单次请求没有收到响应，但这并不代表通信失败。

主机工具现在对只读 opcode 0～3 支持一次重试，因此：

`ATHENA-DIAG opcode=2 page=... missed; retrying once.`

属于工具针对已知 firmware rate limiter 的正常处理，不应被误认为 CAN 丢包或硬件故障。

---

## 4. SAFE_DIAGNOSTIC 已验证能力

SAFE_DIAGNOSTIC 镜像的作用是提供完全禁止功率级的安全诊断环境。

已验证镜像：

`artifacts/athena_safe_diagnostic_factory_can_96f03a7a/motorcontrol.bin`

大小：

`29,380 bytes`

SHA-256：

`96f03a7a64a6f450734f5bc851731691d556f9e7869243d7ee35420eb34aee52`

该镜像已经完成：

- CAN `ping`
- `info` pages 0～4
- `snapshot` pages 0～19
- watch counter pages 9～13
- 安全状态读取
- 编码器状态读取
- ADC 状态读取
- CAN error counter 读取

典型安全状态：

`SAFE=1`
`PA11=0`
`POEN=0`
`CH=000`
`CAN_ERR=0`

这证明“MCU + CAN + 诊断协议 + 基础采样链路”可以在完全不打开功率级的情况下独立验证。

---

## 5. 编码器 AS5047P：已经完成实际验证

编码器是当前整个 bring-up 中非常重要的已验证基础能力。

典型正常状态：

`enc=1`

并且 snapshot 可以稳定得到：

- `encoder_frame_raw14`
- `encoder_count`
- `encoder_turns`
- `encoder_diaagc_mag`

同时诊断计数器可以看到：

`encoder_parity = 0`

`encoder_ef = 0`

`encoder_jump = 0`

`spi_timeout = 0`

这说明编码器 SPI2 数据链路和协议解析均已经实际工作，而不是仅仅通过编译检查。

### 5.1 ST-LINK 对启动时序的影响

曾经发现一个重要现象：

仅拔掉控制板上的 ST-LINK SWD 线后，MCU 和 CAN 仍然工作，但编码器变成：

`enc=0`

而将 ST-LINK USB 本身完全拔掉后，早期版本甚至出现 LED 停止、CAN 不响应。

进一步排查后发现，根因更接近启动/上电时序，而不是“ST-LINK 是正常运行必需品”。

随后将编码器启动等待从 10 ms 增加到 100 ms，并把 warm-up 改成最多 3 次完整尝试、每次间隔 20 ms。最终通过完整断电、断开 ST-LINK 后重新上电验证，编码器重新恢复正常。

因此目前可以明确记录：

**ST-LINK 不是运行时 CAN 或 DRV 通信的必需条件；此前 ST-LINK 相关现象主要暴露了启动时序脆弱性。**

### 5.2 最终冷启动证据

断开 ST-LINK 后重新上电，最终可以获得：

`SAFE=1 nFAULT=0 PA11=0 ... enc=1 adc=1`

随后 snapshot 可以连续读取编码器 raw frame、count、turns 等数据，说明 MCU、编码器和 CAN 均能在无 ST-LINK 条件下正常启动。

---

## 6. ADC：已经完成实际验证

正常 snapshot 中：

`adc=1`

并能够返回：

- `phase_adc_b_c`
- `vbus_adc_raw`
- `temperature_adc_raw`
- Hall/辅助 ADC 数据

同时诊断计数器：

`adc_timeout = 0`

这意味着 ADC 初始化、触发和读取链路已经通过实际硬件验证。

当前尚未完成的是：

**相电流 ADC 的实际物理比例、极性、零点和三相映射是否完全正确。**

也就是说，“ADC 能读”已经验证，但“ADC 数值和真实电流的工程量对应关系”仍属于后续电机控制标定工作。

---

## 7. DRV8323RS 唤醒：已经完成实际验证

这是本次 bring-up 最关键的硬件验证之一。

最终一次成功的 `drv-wake` 实测结果：

`op=6 page=0 info status=OK payload=0x00000001`

这代表 DRV wake/configuration verification 成功。

同时读取到：

`DCR = 0x00A0`

`CSACR = 0x02DC`

`OCPCR = 0x0415`

`FSR1 = 0x0000`

`FSR2 = 0x0000`

8 次 SPI 事务全部：

`status=OK`

### 7.1 SPI TX/RX 实测值

8 次事务的实际结果如下：

| 序号 | 目的 | TX | RX |
|---|---|---:|---:|
| 0 | DCR write | `0x10A1` | `0x0000` |
| 1 | CSACR write | `0x32DC` | `0x0283` |
| 2 | OCPCR write | `0x2C15` | `0x0159` |
| 3 | FSR1 read | `0x8000` | `0x0000` |
| 4 | FSR2 read | `0x8800` | `0x0000` |
| 5 | DCR read | `0x9000` | `0x00A0` |
| 6 | CSACR read | `0xB000` | `0x02DC` |
| 7 | OCPCR read | `0xA800` | `0x0415` |

因此可以确认：

**GD32 → DRV8323 SPI 写入正常；DRV8323 → GD32 SPI 回读正常；寄存器配置可以被实际读取回来；FSR1/FSR2 清零；ENABLE/PA11 唤醒时序能够让 DRV 进入可通信状态。**

### 7.2 DRV SPI 时钟

中途曾出现所有 RX 都为：

`0xFFFF`

排查过程中发现早期 SPI1 使用 APB1/8，在 60 MHz APB1 下为 7.5 MHz，高于 DRV8323 的 5 MHz SPI 上限。

之后将 DRV 专用 SPI1 调整为 3.75 MHz，并加入 nSCS 建立/保持延时以及帧间至少约 400 ns 的高电平间隔。

最终实测成功的 SPI 结果证明这一修改后的 SPI 配置可正常工作。

编码器 SPI2 没有跟着修改为 DRV 的参数，二者保持独立。

### 7.3 PA11 唤醒等待时间

早期 `drv-wake` 使用 1 ms 的 PA11 高电平等待时间时，SPI RX 全部为 `0x0000`。

恢复为 10 ms 后，RX 从全零变成有效 DRV 数据。

因此：

**DRV8323 唤醒后首个 SPI 事务不能简单地立即执行；当前已验证的实现采用 10 ms 安全等待窗口。**

---

## 8. drv_ready 状态保持问题

曾经出现：

`drv-wake` 已经返回成功，但随后 page 27 却显示：

`drv_ready=0`

这不是 DRV 失效，而是 firmware 状态机错误。

原来的 `inject_handle_can()` 将后续只读诊断帧当作“其他控制帧”，从而清除了刚刚建立的 `drv_ready`。

已经修正为：

- 诊断 opcode 0～3 只读请求不会清除 `drv_ready`
- 新的 `drv-wake` 会重新建立 `drv_ready`
- `stop`、未知控制帧等才会明确清除 readiness

这是一个重要的软件验证经验：

**诊断查询本身不能改变被测系统状态。**

---

## 9. DRV 普通 `drv` 读取与 drv-wake 的区别

早期直接执行普通 `drv` 时出现过：

`0xFFFF`

不能简单理解为“DRV SPI 坏了”。

最终验证证明，DRV 的正常寄存器访问需要满足 ENABLE/VM/唤醒状态，而且 DRV 的寄存器读取不能脱离正确的 PA11 唤醒时序独立解释。

因此最终诊断工具增加了专门的 `drv-wake`：

1. 禁止 PWM 主输出；
2. 三路比较值设为安全全低；
3. 拉高 PA11；
4. 等待 10 ms；
5. 写入受限配置；
6. 读取 FSR1/FSR2；
7. 读取 DCR/CSACR/OCPCR；
8. 验证所有 SPI 事务状态；
9. 拉低 PA11；
10. 恢复安全 idle。

后续正常固件应继承这个硬件时序，而不是直接复制早期普通 `drv` 读取方式。

---

## 10. 安全状态：已经通过实际验证

BRINGUP_INJECT 的安全边界经过多次测试。

典型安全状态：

`SAFE=1`

`nFAULT=0`

`PA11=0`

`enc=1`

`adc=1`

`POEN=0` 或特定版本中的 `POEN=1`

以及三个 PWM channel 保持安全比较值。

最终又进一步收紧了 idle 状态，正常原则为：

`PA11=0`

`TIMER0 primary output disabled`

三个比较寄存器保持安全值。

这里必须区分：

`POEN=0` 是更严格的电气 idle 状态；

`PA11=0` 是 DRV gate enable 的关键独立门控。

因此不能单纯因为 POEN 的值发生变化就认为 DRV 已经通电。

---

## 11. nFAULT 处理：已经完成故障路径验证

测试过程中真实触发过 DRV fault。

早期曾出现一个软件误判：`drv-wake` 成功后主动拉低 PA11，DRV 在 gate disable 后产生 nFAULT 边沿，而 EXTI 中断无条件把这个边沿记录成：

`SAFETY_FAULT_GATE_DRIVER = 0x20`

这导致第一次 inject 被错误拒绝。

随后将 nFAULT 的锁存条件修改为：

**只有在 PA11 已经处于高电平、即 DRV 真正处于 enable 状态时，nFAULT 才作为运行时 gate-driver fault 锁存。**

这样可以区分：

- DRV 被主动关闭后的正常 nFAULT 状态；
- DRV 已经 enable 后出现的真实故障。

同时，真实 nFAULT 在受控 PWM 期间仍然会立即关闭功率输出并锁存故障。

---

## 12. 第一次真实 PWM 注入：已经得到明确的故障证据

首次真实受控注入并没有成功驱动电机，而是触发了 DRV fault。

故障瞬间读取：

`FSR1 = 0x0400`

`FSR2 = 0x0040`

根据 DRV8323RS 定义：

`FSR1 bit10 = FAULT`

`FSR2 bit6 = CPUV`

即：

**DRV8323 charge pump undervoltage，电荷泵欠压。**

这不是 VDS OCP，也不是简单的相线短路证据。

### 12.1 故障根因已经定位

问题不是 DRV SPI 唤醒失败。

真实流程是：

1. `drv-wake` 成功；
2. DRV 已经完成配置；
3. `drv-wake` 结束后 PA11 被关闭；
4. 用户随后发送 `inject`；
5. 原来的注入状态机直到第一个 PWM tick 才重新拉高 PA11；
6. 此时 PWM 切换已经开始，DRV charge pump 尚未重新建立；
7. DRV 立即产生 CPUV。

因此这是一个**功率级 enable 时序错误**。

已经修改为：

1. 接收到 inject；
2. PA11 先拉高；
3. PWM 主输出仍然关闭；
4. 三路 PWM 输入保持全低；
5. 等待 10 ms，让 DRV charge pump 建立；
6. 再确认 nFAULT、PA11 和安全锁存均正常；
7. 最后才允许 PWM 主输出和目标 vector。

这条修复已经完成代码、镜像审计和离线测试，但修复后的最终物理注入成功证据在本文件结束处尚未形成。

---

## 13. 注入状态机已经验证的安全特性

BRINGUP_INJECT 不是一个自动运行的电机控制器，而是一个受 CAN 命令严格门控的诊断镜像。

它的安全原则已经实现并验证：

- 上电后不会自动产生电机动作；
- 没有 CAN inject 命令时 PA11 保持关闭；
- 每条 inject 命令只产生一个有限时长脉冲；
- inject 命令需要显式 `--confirm-inject`；
- `drv_ready` 不成立时拒绝注入；
- 编码器无效时拒绝注入；
- ADC 无效时拒绝注入；
- nFAULT 异常时拒绝或中止；
- 电流 ADC 超过限制时中止；
- stop 命令具有最高优先级；
- 未知控制流量能够中止正在进行的注入；
- 注入结束后重新进入安全状态。

---

## 14. 注入测试参数

第一次注入的最低参数：

`vector=0`

`duty=0.5%`

`duration=10 ms`

六个基础 BLDC vector 为：

| Vector | U | V | W |
|---:|---|---|---|
| 0 | 高侧占空比 | 低 | 低 |
| 1 | 高侧占空比 | 高侧占空比 | 低 |
| 2 | 低 | 高侧占空比 | 低 |
| 3 | 低 | 高侧占空比 | 高侧占空比 |
| 4 | 低 | 低 | 高侧占空比 |
| 5 | 高侧占空比 | 低 | 高侧占空比 |

首次测试设计目标不是立即让电机转起来，而是利用：

- 电机本体；
- 编码器；
- 相电流 ADC；
- DRV fault；

来确认：

1. PA11 是否真正控制 gate enable；
2. PWM U/V/W 与实际电机端子的对应关系；
3. ADC B/C 与实际相电流的对应关系；
4. ADC 符号是否正确；
5. 编码器方向是否与电流 vector 对应；
6. 六个 vector 是否产生预期电流模式。

---

## 15. 当前尚未证明的内容

以下内容绝对不能写成“已经验证”。

### 15.1 电机已经正常旋转

尚未形成成功的连续运动证据。

第一次真实注入出现 CPUV fault，已经定位为 charge-pump precharge 时序问题。

修复镜像已经构建，但在本次 rollout 文件结束时没有形成“修复后实际电机正常运动”的新证据。

### 15.2 相序

目前没有可靠的实际运行数据证明：

`PWM U/V/W ↔ 电机三相端子`

的映射已经完全正确。

### 15.3 ADC 电流极性和比例

已经证明 ADC 可以读取，但尚未完成：

- 零点；
- 增益；
- 正负极性；
- B/C 通道与真实相线映射；

的最终标定。

### 15.4 编码器方向

已经证明编码器能读，但是：

“电流 vector 正方向 → 电机机械角正方向”

还没有通过最终运动测试完成确认。

### 15.5 FOC 参数

包括：

- KT；
- PPAIRS；
- 电流环参数；
- 速度环参数；
- 电角度零点；
- phase order；
- current scaling；

均不能因为底层通信已经验证就认为正确。

### 15.6 正常 MIT CAN 运动闭环

正常固件的软件路径已经完成大量修改和离线审计，但最终：

`MIT CAN → CAN ISR → MOTOR_MODE → gate enable → PWM → FOC → 电机反馈 → CAN reply`

还没有完成最终实机闭环。

---

## 16. 正常固件已经继承的已验证能力

后续正常固件不应重新发明这些底层机制。

目前正常固件已经逐步迁移：

### CAN

采用已经验证的 1 Mbps factory timing，并使用硬件宽接收、ISR 软件严格判断：

- standard frame；
- data frame；
- DLC=8；
- CAN_ID。

这样避免 GD32 硬件过滤器中 `CAN_ID << 5` 的历史兼容问题。

### 编码器

正常固件已经加入：

- 上电等待；
- warm-up；
- 多次重试；
- SPI timeout；
- parity；
- EF；
- jump；
- invalid 状态处理。

### DRV

正常固件已经继承：

- SPI1 3.75 MHz；
- nSCS 建立/保持延时；
- 10 ms enable settle；
- DCR/CSACR/OCPCR 配置；
- FSR1/FSR2 检查；
- nFAULT 检查；
- 配置回读；
- gate enable 前安全门。

已验证的 OCPCR 配置值：

`0x0415`

### 功率级

正常固件采用：

- 启动默认 PA11 低；
- TIMER0 primary output disabled；
- MOTOR_MODE 需要显式命令；
- enable 过程不能在 TIMER ISR 中阻塞等待 SysTick；
- timeout 自动关闭 driver；
- timeout 后必须重新收到显式 motor command 才能重新 arm。

---

## 17. 正常固件配置区问题：已经定位并修复

这是正常应用 CAN 无回复问题中的关键根因之一。

正常固件原来直接把：

`0x0803C000`

当作配置区读取。

但实际 Flash 内容是原厂 firmware 的代码残留，而不是 Athena 配置。

实际读取出的前几个 word：

`PHASE_ORDER = 0x6C61682D`

`CAN_ID = 0x000D0A6C`

`CAN_MASTER = 0x6F746F52`

`CAN_TIMEOUT = 0x61562072`

旧代码只判断 `CAN_ID != -1`，所以错误地把：

`0x000D0A6C`

当成合法 CAN ID。

于是正常 CAN ISR：

`rx_sfid == CAN_ID`

永远不会匹配发送的：

`0x001`

这就是为什么此前发送：

`t00187fff7ff0000007ff`

只能看到回显，而没有出现预期的：

`t0006...`

### 17.1 新配置格式

已经增加：

- magic；
- format version；
- payload length；
- CRC32；
- 参数范围检查。

无效配置不会修改 Flash，而是在 RAM 中使用默认值。

默认关键参数：

`CAN_ID = 0x001`

`CAN_MASTER = 0`

`CAN_TIMEOUT = 1000`

配置保存采用安全提交顺序：

1. 写 payload；
2. 写 version/length/CRC；
3. 最后写 magic；
4. 完整 readback；
5. 验证成功后才切换 active slot。

因此掉电或半写不会把损坏配置当成有效配置。

---

## 18. 正常固件离线审计状态

正常镜像已经完成：

- protocol tests；
- AS5047 protocol tests；
- motor gate tests；
- config store tests；
- UC12 tool self-test；
- flash-tool self-test；
- normal image symbol audit；
- 双隔离构建一致性检查。

正常镜像审计要求：

- 不包含 `BRINGUP_INJECT`；
- 不包含 `inject_handle_can`；
- 不包含 `inject_service`；
- 不包含 `inject_timer_tick`；
- 必须包含正常 motor gate、DRV、配置验证等关键符号。

最新正常镜像候选：

`artifacts/athena_normal_app_audit_20260821/motorcontrol.bin`

大小：

`52,372 bytes`

SHA-256：

`538da9dc8aac7c4e144de6d6b8f126fbc8f7fee348ef1976ed6b9a6e72330afa`

该镜像在文档对应阶段属于“已审计但尚未硬件接受的候选”，不能把离线测试写成实机运动验证。

---

## 19. Flash 安全机制已经验证

`tools/athena_safe_flash.sh` 已经经过多次修改和自测。

`flash-inject` 的核心保护：

- 刷写前完整 Flash 双读；
- 比较两次 Flash 读回结果；
- 保存当前配置区；
- 保存 Option Bytes；
- 应用区按 2 KiB 页逐页 erase/write/verify；
- 任一页失败立即停止；
- CPU 保持 halted；
- 刷写后重新完整 readback；
- 配置区前后比较；
- Option Bytes 前后比较；
- 不执行 mass erase；
- 不写 Option Bytes；
- 镜像 SHA 必须与明确确认值一致。

这套流程本身已经通过脚本自测。

---

## 20. 重要失败记录及其意义

### 20.1 “CAN 不通”——实际上是主机 ACK 解析错误

结论：不要重新怀疑 CAN 硬件。

### 20.2 SPI RX 全 0——实际上是 DRV 唤醒等待过短

1 ms 等待时 RX 全零；10 ms 后恢复正常。

结论：DRV8323 有明确启动时序要求。

### 20.3 SPI RX 全 `0xFFFF`——早期 SPI 时钟超过 DRV 上限

7.5 MHz 超过 5 MHz；改成 3.75 MHz 后最终成功。

结论：SPI 参数必须严格遵守 DRV8323 限制。

### 20.4 `drv_ready=0`——诊断查询清掉 readiness

不是 DRV 失败，而是 firmware dispatch 状态机 bug。

结论：只读诊断不能修改被测状态。

### 20.5 inject 被 `0x20` 拒绝——nFAULT disable edge 被误判

不是 DRV wake 失败，而是 PA11 关闭后的正常 nFAULT 边沿被 EXTI 当成故障。

结论：nFAULT 需要结合 PA11 状态解释。

### 20.6 第一次真实 PWM 触发 CPUV

`FSR1=0x0400`

`FSR2=0x0040`

结论：DRV charge pump undervoltage，根因是 wake 后关闭 PA11，inject 第一 tick 才重新 enable，缺少重新预充时间。

这是真实硬件故障证据，也是目前最重要的功率级时序发现。

---

## 21. 后续正常固件开发必须遵守的验证边界

以后看到类似问题时，应按照以下顺序判断：

第一层：MCU 是否运行。

证据包括 LED/SysTick、CAN ping、uptime、sample sequence。

第二层：CAN 是否真正双向工作。

不能只看 USB-CAN 回显，要看 MCU firmware response。

第三层：编码器是否有效。

必须确认 `enc=1`，并观察 parity/EF/jump/timeout。

第四层：ADC 是否有效。

必须确认 `adc=1` 且 ADC timeout 为 0。

第五层：DRV 是否能被安全唤醒。

必须使用经过验证的 PA11 + 10 ms + SPI readback 流程。

第六层：DRV 配置是否真实正确。

必须确认：

`FSR1=0`

`FSR2=0`

`DCR`

`CSACR`

`OCPCR`

均与配置一致。

第七层：功率级是否能够在 charge-pump 已建立后安全打开。

这是当前下一阶段的关键。

第八层：低占空比 vector 是否产生预期电流。

此时才开始确认相序和 ADC。

第九层：编码器是否产生对应机械角变化。

确认电角度方向和机械方向。

第十层：最后才进入闭环 FOC/MIT 正常运行。

---

## 22. 最重要的最终结论

目前已经不能再把项目描述成“CAN 已经通了，下一步试试电机”这么简单。

更准确的状态是：

**MCU 运行链路已经验证。**

**CAN 双向通信已经验证。**

**UC12 主机协议已经验证。**

**编码器 SPI2 和有效数据已经验证。**

**ADC 采样链路已经验证。**

**无 ST-LINK 冷启动已经验证。**

**DRV8323 ENABLE 唤醒已经验证。**

**DRV8323 SPI1 已经验证。**

**DRV8323 DCR/CSACR/OCPCR 配置写入和回读已经验证。**

**DRV8323 FSR1/FSR2 正常状态已经验证。**

**安全 gate、nFAULT、PA11、PWM 关闭路径已经验证。**

**第一次真实 PWM 注入已经实际触发过 DRV CPUV，并且故障原因已经定位到 charge-pump precharge 时序。**

因此目前真正剩余的问题已经从“硬件是否能工作”缩小到了：

**正确的功率级启动时序 → 最低占空比 PWM → 电流采样方向/比例 → 相序 → 编码器方向 → FOC/MIT 闭环。**

这意味着此前的大量 bring-up 工作并不是重复走弯路，而是在把开源工程中“理论上适用于某个板卡”的代码，逐项转换成“这块实际小米 CyberDog 一代控制板已经通过真实硬件证明有效”的事实。

后续任何正常固件修改，都应该以本文档中的实测值和状态边界作为回归基准，尤其是：

`CAN 1 Mbps`

`SPI1 3.75 MHz`

`DRV wake settle 10 ms`

`DCR = 0x00A0`

`CSACR = 0x02DC`

`OCPCR = 0x0415`

`FSR1 = 0`

`FSR2 = 0`

`enc = 1`

`adc = 1`

`PA11 = 0` 为默认安全状态。

