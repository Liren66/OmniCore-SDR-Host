# PPS 时钟频率监控与 40 MHz 驯服

PPS 计数器测量 AD9361 数字接口的 DATA_CLK 频率，本文介绍这项监控和 40 MHz 驯服；它本身不提供 UTC 时间。用 PPS 装载样点时间、定时收发见 [硬件时间戳](timestamps.md)。默认关闭；不需要外部 PPS 也可以正常启动和收发。在 PA4 调谐硬件上（SDK `vcxo=1`），只用 PPS、不用外部 10 MHz 时，CH32 会按这些计数驱动 PA4 DAC 驯服板载 40 MHz，见下文 [40 MHz 调谐模式](#40-mhz-调谐模式sdk-vcxo1)。

## 使用

```powershell
och get pps.input-enabled
och set pps.input-enabled on
och get pps.status
och get pps.status --json
och set pps.input-enabled off
```

SDK 原指令：`pps_input_en?`、`pps_input_en=0|1`、`pps_status?`。`system_status?` 也包含 PPS 状态。`system_info?` 的 `pps_monitor=1` 表示 SDK 已连接支持 PPS 的 FPGA；`pps_adjustment=0` 表示没有调节功能。旧固件/旧比特流会返回不支持，不伪造有效测量。

开启只解除 PPS 中断屏蔽并启动统计，关闭时屏蔽中断并清空本次统计；FPGA 内部输入采样与计数模块仍存在。打开或关闭监控不暂停 IQ 流、不改采样率、不切参考源。选择在关闭/重新打开 OCH 后保留，在 `rf_reinit!` 后保留；SDK 重启恢复关闭。

## 状态和数据含义

| 字段/状态 | 含义 |
|---|---|
| `disabled` | 监控已关闭 |
| `waiting` | 开启或重新配置后等待两个连续有效脉冲 |
| `valid` | 最近完整 PPS 间隔产生有效频率 |
| `no_signal` | 开启后或最后脉冲之后超过 1.5 秒未收到新脉冲 |
| `invalid_interval` | 脉冲间隔或计数异常，包括丢脉冲后第一次恢复 |
| `clock_unavailable` | RF 时钟未就绪，监控选择保留，测量暂停 |
| `signal_present` | 最近 1.5 秒收到过脉冲，不等于合格的 1 Hz 信号 |
| `frequency_valid` | 必须为 1 才能使用频率和频偏 |
| `nominal_hz` | 根据当前时钟配置推算的名义 DATA_CLK，非测量值 |
| `measured_hz` | 假设外部脉冲为准确 1 Hz，由 FPGA 周期计数获得的 DATA_CLK 频率 |
| `error_ppm` | `(measured_hz / nominal_hz - 1) × 10^6`；无效时协议返回 `unknown` |
| `pulse_count` / `invalid_count` | 本次启用期间收到的 IRQ 数/被拒绝的间隔数，32 位计数可回绕 |
| `age_ms` | 距最后脉冲的毫秒数；4294967295 表示开启/重新测量后尚无脉冲 |
| `raw_counter` | 有效时的 FPGA 原始计数，修正零起点后 `measured_hz = raw_counter + 1` |
| `hardware_timeout` | HDL 自带超时标志，仅作诊断；门限依赖 DATA_CLK，软件状态采用固定 1.5 秒 |

当前物理接口固定为 2R2T LVDS，DATA_CLK = 4 × 每通道采样率。30.72 MSPS 对应名义 122.88 MHz，61.44 MSPS 对应 245.76 MHz；USB 只选一个 RX 通道不会改变这个关系。监测值不是板上 40 MHz 参考时钟的直接读数。

首次 PPS 只建立起点，不发布频率。PS 定时器只检查 IRQ 间隔是否在 0.9–1.1 秒，硬件计数还须在名义频率 ±10% 内；PS 接收时刻不用于精确算频率。下一次连续合格 PPS 才有效。丢失输入时立即停止发布过期频率，`measured_hz=0`、`error_ppm=unknown`；零表示无效占位，不表示真的测得 0 Hz。改采样率、FIR/时钟重配和 RF 重初始化后重新等待两个脉冲。

板上 PPS 接口是 J6 的 PPS_EXT（原理图连接器文字为 10M_EXT），10 MHz 外部频率参考则接 J3。输入电平必须与实板 Bank 供电匹配。OCH 不改变 FPGA 的 LVCMOS25 约束或板上电压。

## OCH 的 LOCK 显示

从 0.3.4 起，参考状态、PPS 状态及系统状态统一使用 PPS 测频判定 `locked`，并标注 `lock_source=PPS`、`lock_tolerance_ppm=1`。

| 监控情况 | LOCK 显示 |
|---|---|
| 已开启，最近有效频率与名义 DATA_CLK 相差不超过 ±1 ppm（包含边界） | `locked=1` |
| 已开启，测量有效但超出 ±1 ppm | `locked=0` |
| 关闭、无输入、等待两个有效脉冲、无效间隔、时钟不可用、超时或查询失败 | 隐藏 LOCK |

判定使用整数频率差，避免 ppm 显示舍入影响边界；每次有效测量更新，不累计多秒确认。`frequency_valid=1` 的 SDK 校验范围是 ±10%，不能直接当作锁定。

这是相对于输入 PPS 的频率容差标志，不是相位锁定、ADF4002 锁定反馈或绝对频率精度保证。只开启外部 10 MHz 时不需要 PPS，ADF4002 实际锁定仍需观察 D5。监控不调节时钟。

CLI 的普通输出及 JSON `reply` 隐藏无法观测的硬件 lock/LED 字段；规范命令 JSON `raw_reply` 和底层 `och_command` 保留原始 SDK 回复，供协议诊断。界面应使用规范 `reply` 或 C API 的 `lock_known/locked`，不要显示原始 SDK 的 `locked=-1`。

## C/C++ 接口

```cpp
och::device device;
device.set_pps_enabled(true);
auto status = device.pps_status();
if (status.frequency_valid) {
    // status.measured_hz 和 status.error_ppm 有效。
}
if (status.lock_known) {
    // 显示 status.locked：1 = 频率在 ±1 ppm 内，0 = 超出。
} else {
    // 隐藏 LOCK。
}
device.set_pps_enabled(false);
```

C ABI：`och_set_pps_enabled(device, 0|1)`，`och_get_pps_status(device, &status)`；`och_pps_status.struct_size` 必须设置。无效频率在 C 结构中以 `frequency_valid=0` 和零数值表示。0.3.4 在原保留槽加入 `lock_known/locked`，`och_pps_status` 仍为 96 字节，`error_ppm` 偏移仍为 56，保持 ABI 1。`lock_known=0` 时 `locked=-1`，界面必须隐藏。`och_reference_status.lock_known/locked` 同样表示 PPS 频率监控结果；参考 getter 将不支持/失败的可选 PPS 查询降级为未知，显式 PPS getter 仍报告错误。

同进程 API 可以在 RX/TX 流运行期间调用，也允许持有 RX 租借缓冲。查询/开关失败不会停流或使 RF 配置失效。原始 `och_command` 的写操作仍要求没有流；正在收发的应用应使用上述类型化接口，不能另开 CLI 抢占设备。

## 板级实现与验证

新比特流包含修订号 2 的 PPS 接收 IP。PPS 中断连接 concat In0 → IRQ_F2P[0] → GIC 61，SDK 显式采用此板级映射；当前 BSP 自动生成的 `XPAR_FABRIC_AXI_AD9361_GPS_PPS_IRQ_INTR=91` 与实线不一致，91 留给现有 CH32 中断。驱动共享已有 GIC，使用上升沿触发，同时解除 RX/TX 两侧 PPS 屏蔽。没有修改 CH32H417 工程。

接入真实外部 10 MHz 和 PPS 后，D5 常亮；30.72 MSPS 下稳定读数为 122880000–122880001 Hz、频偏 0–0.008 ppm。该结果相对于输入 PPS，不是独立仪表的绝对精度验收。计数功能背景可参阅 [ADI AXI AD9361 文档](https://analogdevicesinc.github.io/hdl/2023_R2/library/axi_ad9361/index.html)，具体计数修正和中断映射以本项目 RTL/连线为准。

## 40 MHz 调谐模式（SDK `vcxo=1`）

新硬件把 CH32H417 的 PA4（DAC1_OUT）接到 ADF4002 电荷泵输出，即 40 MHz VCTCXO 的调谐节点（CP→R109 100 kΩ→Vc，R101/R123 1 MΩ/1 MΩ 偏置到 1.65 V），原 PA4 LED 已去掉。两个驱动源不能同时工作，模式由两个开关组合：

| 模式 | `clock.ref-source` | `pps.input-enabled` | ADF4002 | PA4 | LOCK |
|---|---|---|---|---|---|
| 0 | internal | off | CE 低、电荷泵高阻 | 高阻 | 隐藏；Vc 由偏置电阻保持 1.65 V |
| 1 | external | off | 锁相到 J3 10 MHz | 高阻 | 不读锁定（和以前一样默认锁定，看 D5） |
| 2 | internal | on | CE 低、电荷泵高阻 | CH32 DAC 闭环 | `lock_source=CH32_PPS_LOOP` |
| 3 | external | on | 锁相到 J3 10 MHz | 高阻 | PPS ±1 ppm 频率检查，仅作参考 |

**切换规则。** 关闭电荷泵立即生效。打开电荷泵（进入 1 或 3）时，SDK 先让 CH32 释放 PA4，收到 CH32 对本次请求的 `pa4=0` 应答后才开启 ADF4002；`clock_ref_source=1` 先回 `pending=1`，OCH 轮询 `vcxo_status?` 最多 5 s。3 s 内没有应答时，电荷泵保持高阻，并报 `error=-116`，`och set clock.ref-source external` 失败。CH32 上电和复位后 PA4 都是高阻，只有收到 Zynq 的模式 2 才驱动。

**CH32 环路（模式 2）。** Zynq 在每个 PPS 边沿上传一行计数，其中期望值按 BBPLL 整数和小数字精确计算。CH32 的处理顺序：

1. 第一次进入时 DAC 写 2048，与偏置电压相同，接管无跳变。
2. CAL：DAC 步进 400 LSB 测量斜率 K（ppb/LSB），约 36 s。这一步会让 40 MHz 偏移几百 ppb 到几 ppm，具体取决于晶振压控斜率。
3. ACQ：每 8 s 修正一次频率。
4. TRACK：相位 PI 环，τ=15 s、ζ=0.707，把 PPS 相位误差压到 0。DAC 按 1 Hz 做一阶 Σ-Δ 取整，把舍入误差带到下一秒，平均频率等于 PI 输出的小数值，避免在两个码之间来回跳的极限环。
5. 超过 3 s 没有新样本时进入 HOLD，DAC 保持不变；单次跳变超过 500 ppb 视为毛刺，丢弃该样本并把相位清零。相位误差超过 ±2 µs 时记为一次相位滑动（phase slip）：只把相位清零，频率积分和 DAC 保留，继续 TRACK，不再退回 ACQ。

再次进入模式 2 时，直接从上次的 DAC 值和 K 开始 ACQ。环路在 CH32 本地运行，Host 不读 EP2 时也照常工作。改采样率、FIR、`rf_reinit!` 期间没有计数，环路处于 HOLD。

**锁定判断。** `locked=1` 表示 CH32 处于 TRACK，且相位误差在 ±1000 ns 以内。实测斜率约 10.6 ppb/LSB。开始或停止流传输时板子温度变化，40 MHz 会在一分钟内漂移约 100–140 ppb；τ 取 15 s，按实测斜率仿真，这种热瞬态的相位峰值约 550 ns，2 ppb/min 慢漂约 80 ns（τ=40 s 时热瞬态会超过 2 µs）。Σ-Δ 只在 1 Hz 环路速率上做，不做更快的抖动，以免在射频本振上产生调制边带。

```powershell
och set pps.input-enabled on        # 内部参考 → 模式 2，CH32 驯服
och get clock.vcxo-status
och set clock.ref-source external   # → 模式 3，CH32 先释放 PA4
```

`och_get_vcxo_status` / `device.vcxo_status()` 返回 `och_vcxo_status`：

- `requested_mode` / `applied_mode` / `pending` / `error`：模式请求和执行情况。
- `pll_enabled`：ADF4002 电荷泵是否开启。
- `bridge_alive`：3 s 内收到过 CH32 应答。
- `pa4_driving`、`state`（OFF/CALIBRATE/ACQUIRE/TRACK/HOLD）、`dac_code`、`k_ppb_per_lsb`、`error_ppb`、`phase_ns`、`samples`、`locked`：只有 `bridge_alive=1` 时有效，否则 `state=UNKNOWN`、`pa4_driving=-1`、`dac_code=-1`。

在模式 2 下，`och_reference_status.lock_known/locked` 也改用这个结果。PPS 开关在流运行期间可以直接切换，不停流。
