# OmniCore Host 命令目录

规范命令使用小写英文、点号分组和连字符：采样率始终写 `sample-rate`，射频本振始终包含 `lo`。`och commands` 查看完整目录、参数范围与操作类型，`och commands --json` 供脚本读取元数据；这两个命令无需连接设备。

```powershell
och get rx.sample-rate
och set clock.sample-rate 30.72M
och set rx.lo-frequency 2.4G
och set tx.lo-frequency 2.4G
och set tx1.attenuation 10.25
och set rx1.gain-mode manual
och set rx1.gain 30
och get rx.dc-correction
och set rx.dc-correction on
och set rx.iq-correction on
och get clock.chain
och get rf.register 0x037
och action usb.clear
```

`get` 查询，`set` 设置，`action` 执行动作。部分命令只支持其中一种操作。多参数按表中顺序用空格分隔。设置操作不自动启动发送，只有显式设置 `tx.enabled on` 才请求打开 TX；USB 数据源还必须先有数据预填充。常规采集/发送请使用 `och rx`、`och tx` 和 `och bench`，由基座协调缓冲和启动顺序。

所有规范频率和带宽以 Hz 为单位，增益/衰减以 dB 为单位，DDS 幅度以 -1..1 的比例为单位。数字支持十进制、小数、科学计数法及 `k`/`K`、`M`、`G` 后缀，例如 `30.72M` 等于 `30720000`。拒绝 NaN、Inf、尾随字符、空格嵌入、错误参数数量和不符合步进的数值。只有寄存器地址与桥控制位掩码额外允许 `0x` 十六进制。

RX/TX 共用采样时钟。`rx.sample-rate`、`tx.sample-rate` 和 `clock.sample-rate` 都会影响两方向；并非两路独立采样率。实际可用速率还受 FIR 插值/抽取、ADC/DAC 时钟链限制。三种规范设置都使用新 SDK 的 `clock_sample_rate=` 时钟链生成和核验路径；对应的旧 raw 设置仍保留原行为。FIR 装载请使用专用 `och fir` 命令；自定义时钟链使用专用 `och clock` 命令，不应手工拼接上传/提交事务。

`get`/`set` 返回的主要标量字段会换算为规范单位，并使用规范名称。例如 SDK 的 `tx1_attenuation=10250` 会显示成 `tx1.attenuation=10.25`。多字段状态和额外诊断内容保留 SDK 格式。返回值是设备报告的值，可能有硬件量化；不要假设它必然等于请求值。

## 射频与通道

表中 `get/set` 对应同一 SDK 名称后缀 `?`/`=`；仅 get 的命令不会提供设置入口。

| 规范名称 | 操作 | 参数、单位和约束 | SDK 原名 |
|---|---|---|---|
| `rx.sample-rate` | get/set | 整数 Hz，1..61,440,000；共享时钟，FIR 决定有效下限 | 查询 `rx_samp_freq?`，设置 `clock_sample_rate=` |
| `tx.sample-rate` | get/set | 同上，改变两方向时钟 | 查询 `tx_samp_freq?`，设置 `clock_sample_rate=` |
| `rx.lo-frequency` | get/set | 整数 Hz，70,000,000..6,000,000,000 | `rx_lo_freq`，SDK 单位 MHz |
| `tx.lo-frequency` | get/set | 同上 | `tx_lo_freq`，SDK 单位 MHz |
| `rx.lo-source` | get/set | `internal`/`0`、`external`/`1`；外部名义 RF 限 4 GHz，物理输入为 2 倍 | `rx_lo_source` |
| `tx.lo-source` | get/set | 同上；与 RX 独立选择 | `tx_lo_source` |
| `rx.rf-bandwidth` | get/set | 整数 Hz，200,000..56,000,000 | `rx_rf_bandwidth` |
| `tx.rf-bandwidth` | get/set | 同上 | `tx_rf_bandwidth` |
| `rx.dc-correction` | get/set | 布尔值；两个 RX 通道的 RF 与基带 DC 跟踪校正 | `rx_dc_correction` |
| `rx.iq-correction` | get/set | 布尔值；两个 RX 通道的正交跟踪校正 | `rx_iq_correction` |
| `rx.channels` | get/set | `off`/`0`、`ch1`/`1`、`ch2`/`2`、`both`/`3`/`1,2` | `rx_channel` |
| `tx.channels` | get/set | 同上 | `tx_channel` |
| `rx1.gain-mode` | get/set | `manual`/`0`、`fast`/`1`、`slow`/`2`、`hybrid`/`3` | `rx1_gc_mode` |
| `rx2.gain-mode` | get/set | 同上 | `rx2_gc_mode` |
| `rx1.gain` | get/set | 整数 dB，-3..76；当前频段可能进一步限制，手动增益模式下设置 | `rx1_rf_gain` |
| `rx2.gain` | get/set | 同上 | `rx2_rf_gain` |
| `tx1.attenuation` | get/set | 0..89.75 dB，步进 0.25 dB | `tx1_attenuation`，SDK 单位 mdB |
| `tx2.attenuation` | get/set | 同上 | `tx2_attenuation`，SDK 单位 mdB |
| `tx.source` | get/set | `dds`/`0`、`usb`/`1`；更改前停止 TX | `tx_source` |
| `tx.enabled` | get/set | 布尔值；显式控制发送门控 | `tx_enable` |
| `rx.fir-enabled` | get/set | 布尔值；启用已加载 FIR，建议优先使用完整 FIR 事务 | `rx_fir_en` |
| `tx.fir-enabled` | get/set | 同上 | `tx_fir_en` |
| `rx.fir-profile` | get/set | 兼容内置示例 profile `0`；加载会禁用相应 FIR | `rx_fir_config` |
| `tx.fir-profile` | get/set | 同上 | `tx_fir_config` |

布尔值接受 `off`/`false`/`0` 和 `on`/`true`/`1`。通道掩码不是通道数量：`2` 表示第二通道，`3` 或 `both` 才表示两个通道。TX 关闭时 SDK 会保持实际硬件最大衰减，衰减查询返回保存的发送目标值。

两项 RX correction 规范命令要求 `system_info?` 中 `rx_correction=1`，不支持的旧 SDK 会明确报错。设置使用 typed API，保留另一项开关并回读验证；返回的 `raw_reply` 保留设置后查询的原始状态字段。DC 是 AD9361 的 RF/BBDC 跟踪，IQ 是 RX 正交跟踪；不是软件滤波或 TX IQ 校准。`off` 只关闭连续跟踪更新，不清除已有系数；IQ 关闭时仍启用硬件相位/增益校正路径。原始 SDK 写命令要求流已停止；持续收发应用使用同进程 C/C++ setter，由 OCH 协调暂停/恢复。独立 CLI 仍需要设备独占所有权。

## DDS

TX1、TX2 各有两个 tone，全部 12 个属性都可 get/set。频率以整数 Hz 设置，主机检查 0..4,294,967,295，SDK 再依据当前 TX 采样率检查不得超过 Nyquist 频率。相位为 I 路相位，Q 路保持 I + 270°；允许 0..360°、0.001° 步进，360° 回绕为 0°。scale 为 -1..1、0.000001 步进；两 tone 叠加可能产生削顶。

| 规范名称 | SDK 原名 | 规范单位 |
|---|---|---|
| `dds.tx1.tone1.frequency` | `dds_tx1_tone1_freq` | Hz |
| `dds.tx1.tone1.phase` | `dds_tx1_tone1_phase` | 度 |
| `dds.tx1.tone1.scale` | `dds_tx1_tone1_scale` | 比例；SDK 使用百万分之一单位 |
| `dds.tx1.tone2.frequency` | `dds_tx1_tone2_freq` | Hz |
| `dds.tx1.tone2.phase` | `dds_tx1_tone2_phase` | 度 |
| `dds.tx1.tone2.scale` | `dds_tx1_tone2_scale` | 比例；SDK 使用百万分之一单位 |
| `dds.tx2.tone1.frequency` | `dds_tx2_tone1_freq` | Hz |
| `dds.tx2.tone1.phase` | `dds_tx2_tone1_phase` | 度 |
| `dds.tx2.tone1.scale` | `dds_tx2_tone1_scale` | 比例；SDK 使用百万分之一单位 |
| `dds.tx2.tone2.frequency` | `dds_tx2_tone2_freq` | Hz |
| `dds.tx2.tone2.phase` | `dds_tx2_tone2_phase` | 度 |
| `dds.tx2.tone2.scale` | `dds_tx2_tone2_scale` | 比例；SDK 使用百万分之一单位 |

## 时钟、系统与 USB

隔离版 0.4.0 新增 `och get usb.iq-bits` / `och set usb.iq-bits 8|12`，映射 `iq_format?` / `iq_format=8|12`。8bit 需要匹配 SDK/FPGA、RX/TX mask 都为 3 和 USB TX source；格式改变前双向全停。常规流用 `--iq-bits 8` 自动协调设置。详见 [8bit 扩展](iq8.md)。

| 规范名称 | 操作 | 参数与含义 | SDK 原名 |
|---|---|---|---|
| `clock.chain` | get | 参考时钟和 RX/TX 各 6 级时钟链，Hz；完整链设置走 `och clock` | `clock_chain?` |
| `clock.sample-rate` | get/set | 整数 Hz，1..61,440,000；共享链生成、校验，受 FIR 限制 | 查询 `rx_samp_freq?`；设置 `clock_sample_rate=` |
| `clock.ref-source` | get/set | `internal`/`0` 自由运行、`external`/`1` ADF4002 外部 10 MHz 驯服；AD9361 仍为 40 MHz | `clock_ref_source?` / `=` |
| `clock.ref-status` | get | 参考配置、GPIO 回读；仅 PPS 时 LOCK 来自 CH32 环路，10M+PPS 时为 ±1 ppm 频率 LOCK，无 PPS 隐藏 | `adf4002_status?` |
| `clock.ref-test` | action | D5 闪三次后恢复选源；需人工确认，不能自动证明芯片通信/锁定 | `adf4002_self_test!` |
| `pps.input-enabled` | get/set | `on`/`off` 或 `0`/`1`，默认关闭；内部参考时由 CH32 PA4 DAC 按 PPS 驯服 40 MHz，外部 10 MHz 时仅作参考 | `pps_input_en?` / `=` |
| `pps.status` | get | 输入状态、DATA_CLK 测量频率/频偏；无输入不提供有效频率 | `pps_status?` |
| `clock.vcxo-status` | get | 40 MHz 调谐模式（0 都不用/1 10M/2 PPS/3 10M+PPS）、ADF4002 与 PA4 归属、CH32 PPS 环路状态 | `vcxo_status?` |
| `host.protocol` | get | 控制封装版本和能力 | `host_protocol?` |
| `system.help` | get | SDK 自带完整命令帮助；原单位 | `help?` |
| `system.info` | get | 固件、通道、数据格式、恢复能力 | `system_info?` |
| `system.status` | get | RF、温度、RSSI、收发状态、故障历史 | `system_status?` |
| `rf.register` | get | 一个寄存器地址，0..1023，允许 `0x037` | `register?55` 等 |
| `rf.reinit` | action | 按 SDK 保存配置重新初始化 RF/PL；会中断传输 | `rf_reinit!` |
| `digital.self-test` | action | 停流后运行数字接口测试 | `digital_self_test!` |
| `usb.data-enabled` | get/set | 两个布尔值：RX enabled、TX enabled | `usb_data_en?` / `=` |
| `usb.config` | get/set | CTRL0 整数位掩码 0..127，可十六进制；受 SDK 状态检查 | `usb_config?` / `=` |
| `usb.rx` | set | 三个布尔值：enabled、dual-channel、select-channel2 | `usb_rx=` |
| `usb.tx` | set | 四个布尔值：enabled、dual-channel、select-channel2、run | `usb_tx=` |
| `usb.status` | get | FIFO、错误、吞吐估计和恢复计数 | `usb_status?` |
| `usb.irq` | get | 中断状态、清错与历史 | `usb_irq?` |
| `usb.clear` | action | 请求异步清错；保留历史、不复位 FIFO | `usb_clear!` |
| `usb.stop` | action | 停 USB 路径；USB TX 源模式还会关闭 TX | `usb_stop!` |

`usb.rx`、`usb.tx`、`usb.config` 是高级桥调试入口，不能绕过 SDK 的 TX 显式启用、预填充和状态检查。双通道时 select-channel2 不参与选择。RX 溢出由 FPGA 处理：当前块补零结束，下一块带 GAP 标志，基座据此开始新 epoch 并给出精确丢样数；不会找回丢失的 IQ 数据。

## 旧命令兼容

旧 SDK 名称仍通过原始命令入口保留，例如 `och get rx_samp_freq`、`och set tx1_attenuation 10000`；旧动作可用 `och set 'usb_clear!'`。旧名称保留 SDK 原单位，**不会自动换算**：

| 参数 | 规范命令 | 旧 SDK 命令 |
|---|---|---|
| RX 本振 2.4 GHz | `och set rx.lo-frequency 2.4G` | `och set rx_lo_freq 2400`，单位 MHz |
| TX1 衰减 10 dB | `och set tx1.attenuation 10` | `och set tx1_attenuation 10000`，单位 mdB |
| DDS 幅度 0.25 | `och set dds.tx1.tone1.scale 0.25` | `och set dds_tx1_tone1_scale 250000` |

SDK 原有 `ch32_` 到 `usb_` 的兼容别名仍属于 raw 命令层。规范目录只公布 `usb.*`。带参数的寄存器查询目前按保守策略要求没有流对象，停流并销毁流对象后再查询。FIR/时钟的 begin、data、commit、abort 等内部事务命令不在用户目录中；专用 API/CLI 负责参数校验、顺序、失败处理和回读确认。

目录实现位于 `apps/och/commands.hpp`，包含每项操作、参数数量、单位换算、步进、枚举和支持状态；通过 `och commands --json` 可读取机器可用的完整目录。该目录描述主机入口能力，不能替代 SDK 根据当前硬件状态进行的运行时校验。底层封装和 FIR/时钟事务见 [设备协议参考](protocol.md)。
