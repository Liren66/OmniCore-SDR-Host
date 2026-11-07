# 固件与协议依赖

主机安装器只安装 Windows 程序、主机 C/C++ 开发接口和目标数据口的 libusbK 驱动，不含板级 SDK 源码副本、ELF、BOOT.bin 或 FPGA bitstream，不自动烧写 CH32、SDK、FPGA 或启动 Flash。把软件装到新电脑后仍须连接具有匹配固件的 OmniCoreSDR。

## 0.6.0：成帧传输为唯一格式

OCH 0.6.0 只支持成帧 USB 传输（每个 16 KiB 块带 16 B 头和样点时间），需要 Zynq 固件 [v2026.11.07](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07) 及以上。打开设备时若 `system_info?` 不含 `transport=framed16k`，`och_open` 返回 `OCH_UNSUPPORTED`，提示加载 FPGA `CH32H417_IP` 1.3 与 `SDK_CMD_1.1`。

| OCH | Zynq 固件（BOOT.bin） | FPGA `CH32H417_IP` | SDK 标识 | CH32 桥接 |
|---|---|---|---|---|
| **0.7.1** | 同 0.7.0；每次都能读到 USB-C 电流需 CH32 桥接 1.0.1 | 1.3（`0x01030001`） | `SDK_CMD_1.2_20261107_SYNC` | **1.0.1**（1.0.0 仍可用，USB-C 电流只在上电后第一次可读） |
| 0.7.0 | v2026.11.07 及以上；`och sync` 需要 v2026.11.07.2（SDK_CMD_1.2，FPGA 带 OC_SYNC_AXI 1.0） | 1.3（`0x01030001`） | `SDK_CMD_1.2_20261107_SYNC` | 1.0.0（不变） |
| 0.6.1 | v2026.11.07 及以上，推荐 v2026.11.07.1 | 1.3（`0x01030001`） | `SDK_CMD_1.1_20261107_EVTCOUNT`（v2026.11.07.1） | 1.0.0（不变） |
| 0.6.0 | v2026.11.07 及以上 | 1.3（`0x01030001`） | `SDK_CMD_1.1_20261107_FRAMED` | 1.0.0（不变） |
| 0.5.x | v2026.11.03 及以前 | 1.1 | `SDK_CMD_1.0_*` | 1.0.0 / legacy |

0.6.1 与 v2026.11.07 也能配合使用，只是过载时固件逐条上报事件、不带 `count`（每条记 1）；v2026.11.07.1 合并过载事件并带 `count=`，见 [硬件时间戳](timestamps.md)。

两边必须配套：0.6.0 不能连接旧固件，0.5.x 也不能使用新固件收发 IQ（新固件不再发送 16 KiB 递增起始头）。CH32 桥接固件对数据内容透明，两种组合都可用 1.0.0。

0.5.0 起，CH32 桥接固件 1.0.0 及以上可由用户手动执行 `och firmware update FILE.ocfw` 通过 USB 升级，详见 [桥接固件升级](firmware-update.md)。`och firmware info` 显示桥接固件版本；1.0.0 之前的桥接固件显示 legacy。

本版实际检查/使用以下能力：

| 组件 | 必要能力 | 检查方式 |
|---|---|---|
| ZYNQ SDK | 命令封装协议 v1 | host_protocol? 包含 host_protocol=1 |
| ZYNQ SDK | 成帧 USB 传输（16 B 块头、样点时间、GAP） | system_info? 包含 transport=framed16k |
| ZYNQ SDK 时间扩展 | 时间基准、PPS 对时、定时 RX、带时间的事件 | 使用时间 API 时要求 hw_time=1 |
| ZYNQ SDK FIR 扩展 | 安全分块上传、快照读取、双通道 SPI 校验 | 使用 FIR 功能时要求 system_info? 包含 fir_upload=1 |
| ZYNQ SDK 时钟扩展 | 无降速回退的共享 Fs 和完整链事务 | 使用安全时钟设置时要求 system_info? 包含 clock_config=1 |
| ZYNQ SDK RX 校正扩展 | RF/BBDC 直流及 RX 正交跟踪开关、状态回读 | 使用 RX correction 时要求 system_info? 包含 rx_correction=1 |
| ZYNQ SDK ADF4002 扩展 | 外部 10 MHz 驯服、GPIO 检查和 D5 自检 | 状态/自检要求 adf4002=1；选源还要求 ref_source=1 |
| ZYNQ SDK 多板同步扩展 | REF_OUT、TRIG、SYNC/PPS 沿计数、AD9361 MCS 步骤、time_source=sync | 使用同步功能时要求 system_info? 包含 sync=1 |
| ZYNQ SDK 外部 LO 扩展 | 独立 RX/TX 源、验证和名义频率状态 | 使用 LO source 时要求 external_lo=1 |
| 可选 2TX2RX 8bit 扩展 | SDK 格式命令与 FPGA signed8 路径 | 仅选择 8bit 时要求 iq_format=1 iq8_2t2r=1；默认 12bit 保持旧 SDK 兼容 |
| CH32 | 已有诊断及流清理协议 v4 | USB 厂商请求 0xD0 的诊断版本 |
| FPGA/IP | `CH32H417_IP` 1.3：16 KiB 成帧块、64 位样点时间、事件 FIFO；packed12 / signed8 通道布局不变 | SDK 回报 `fpga=CH32H417_IP_0x01030001`；数据流逐块校验头部 |
| USB | SuperSpeed 或 USB 2.0 High-Speed（480 Mbit/s）；数据端点 01/81，控制端点 02/82 | 枚举及打开时确认；Full-Speed（12 Mbit/s）不支持 |
| Windows 驱动 | 发行包 libusbK 3.1.0.0 | och-driver check；实际安装仍受 Windows 策略约束 |

旧 SDK 即使支持原始串口命令，也可能缺少 OCH 所需的命令封装协议或成帧传输，不能直接假定兼容。版本不满足时基座明确失败，避免把超时/残留回复误认为新命令已成功。

FIR/clock 扩展继续沿用固件名字 SDK_CMD_1.0_20261023，因此不能只比较名字来判断功能。以能力字段为准；规范的 sample-rate setter 使用安全时钟入口，也需要新扩展。协议详细格式见 [设备协议参考](protocol.md)。自定义 FIR/时钟状态保存在运行中 SDK 的 RAM，重新下载 ELF 或断电不保证保留。

RX 校正同样按 `rx_correction=1` 检查，不要求更换 CH32 固件。旧 SDK 缺少该字段时只拒绝新增校正 API/规范命令，原有功能继续按各自能力工作。typed 校正 setter 在暂停流之前完成能力检查；相同设置不造成重启，真正变更使用 OCH 0.3 的协调暂停/恢复机制。

支持 RX 校正的 SDK 冷启动默认启用两项 RX 跟踪；FIR/时钟操作、RF 重建和 rf_reinit 恢复最近成功保存的开关目标。该目标仅保存在 RAM，断电或重新下载程序后恢复默认值。

0.3.2 的 ADF4002 与 LO source API 通过各自能力字段预检，不改变旧 ABI 结构。冷启动默认内部参考和 RX/TX 内部 LO。10 MHz 驯服后 AD9361 的参考频率仍为 40 MHz；MUXOUT 只接 D5，没有处理器反馈，不能提供软件锁定/LED 读回。D5 自检自动确认的范围是 GPIO pad，仍需人工观察闪烁确认芯片响应。这些 0.3.2 扩展不修改 CH32，也不提供 PPS 或跨设备相干同步，详见 [参考与外部 LO](reference-lo.md)。

封装请求在原 SDK 命令前加入 @请求号；响应以相同请求号的 RSP begin/end 包围，结束行给出结果。独立 EVT 可穿插其间，由控制线程分离。这样多行回复、异步错误和连续命令不会靠不可靠的“静默一段时间”猜测完成。本版同时只在控制通道发出一条待完成请求，不是命令并行执行。

CH32 的协议 v4 是使用原有诊断能力，不代表主机安装器为您升级了 CH32 固件。板级 SDK 与 FPGA 工程在 [OmniCore-SDR-Firmware](https://github.com/Liren66/OmniCore-SDR-Firmware) 仓库，独立于 OmniCoreHost。构建、下载及持久化状态由这些板级工程管理；RAM 中已运行的新 ELF 不等于断电后仍会启动该版本。需要长期使用时，请按板级工程流程部署已确认兼容的启动镜像。

打开设备会停止旧 USB 会话并清理缓存，因此不要同时让原上位机、调试脚本和本基座操作同一个数据口。只需检查 Windows 驱动时用 och-driver check，只需枚举时用 och find。

0.3.3 新增 [PPS 频率监控](pps.md)，要求修订号 2 的 PPS IP 比特流和 `pps_monitor=1` SDK。冷启动默认关闭，缺少硬件或固件支持时返回 OCH_UNSUPPORTED。新增 API 保持 ABI 1，仍不修改 CH32 固件。

PA4 调谐硬件（CH32 PA4 与 ADF4002 CP 同接 40 MHz VCTCXO 调谐端）需要三端同时更新：SDK `vcxo=1 pps_adjustment=1`、带 PA4 握手的 CH32 Bridge 固件、本版 OCH。`och_get_vcxo_status` 在旧 SDK 上返回 OCH_UNSUPPORTED，其余接口对旧 SDK 保持原行为。旧 Bridge 固件把 PA4 当 LED 推挽驱动且不回应握手，新 SDK 因此拒绝打开 ADF4002 电荷泵（`error=-116`），外部 10 MHz 设置失败而不会与 PA4 冲突；旧固件也不得用于新硬件的仅 PPS 模式。

## 0.3.5 板级 RGB 与风扇温控

新增 RGB 开关/状态、风扇温控开关/状态和 Zynq 核心温度查询。使用新 FPGA BOARD_CTRL_AXI 及新版 SDK，能力标志为 `board_rgb=1 fan_control=1 core_temp=1`。当前 SDK 默认 RGB 开启、亮度 15%，风扇温控关闭；OCH 0.3.6 同时兼容旧版 25% 固件，亮度以设备回报为准；不改变已有 C ABI 结构或停止 IQ 流。详见 [板级控制及接口](board-control.md)。

## USB 2.0 高速连接

USB 2.0 需要同时使用更新后的 UHSIF 固件与重新构建的 OmniCoreHost。旧固件缺少 USBHS 控制端点和 0xD0 诊断/流清理实现，旧主机程序则会拒绝非 SuperSpeed 连接。

HS/FS 配置描述符与 USB3 使用相同的端点分工：01/81 为 IQ 数据，02/82 为 USART1 控制，83 为调试文本。USBHS 的控制与调试 IN 使用小于最大包长的短包；复位、取消配置和清除端点 HALT 会同步处理桥接状态。OCH 支持 512 B 最大包长的 High-Speed，仍拒绝 64 B 最大包长的 Full-Speed。

编译下载 CH32 固件并重新构建主机程序后，重新插拔设备，依次运行 `och-driver check`、`och find`、`och info`。OCH 0.4.3 在 USB2 下精确显示 **480 Mbit/s（High-Speed, USB 2.0）**，且不再出现控制端点缺失、SuperSpeed 限制或 CH32 diagnostics unavailable。再通过 USB3 连接重复检查，并分别验证低采样率 RX/TX、停止重启与重新插拔。

USB2 连续传输已单独上板测量，配置、持续时间和通过记录见 [USB2 测试结果](usb2.md)。`och info` 的 61.44 MSPS 是射频采样率上限，USB2 吞吐参考见 README 和实测报告，且只与本次固件和通道 / 精度配置匹配。此修改未降低 SDK 的射频采样率范围，也未自动改变用户配置；换主机、USB 拓扑或固件后应重新测试。

## TX FIR 插值 ×2 / ×4

AD9361 驱动写入 TX FIR 系数时，会把 `0x002[1:0]` 临时设为目标插值；旧 SDK 的 `fir_verify_coeff`（`console_commands/sdr_fir.c`）却在写入后的当前模式下回读。TX FIR 关闭（旁路）时，×2 / ×4 滤波器的回读与写入不一致，SDK 判定失败并回滚，返回 `ERR fir_or_clock_apply code=-1 rollback=verified`。因此旧固件无法在关闭状态装载 TX 插值滤波器（含 `och fir tx load`），已开启的 ×2 / ×4 TX FIR 也无法关闭。

修复后的 SDK 在回读 TX 系数时进入 ALERT 状态，把 `0x002[1:0]` 临时设为配置的插值，读完恢复原值；系数仍被逐个核对。修复版已实测：TX ×2 / ×4 在关闭状态装载、开启与关闭均成功，RX / TX 各种因子组合下的采样率就近选择与全双工吞吐测试通过。RX 抽取不受此问题影响。该修复只在板级 SDK 中，主机程序无需改动；固件版本字符串未变，请以能否装载 TX 插值滤波器来确认所用固件。

## RX RSSI 与控制串口波特率

**RSSI。** 新版 SDK 增加 `rssi_enable? / =` 和 `rx_rssi?`，`system_info?` 中新增一行 `rf_capabilities rssi=1 console_baud=<波特率>`。

- **默认关闭。** 冷启动后为 `rssi_enable=0`，此时 `rx_rssi?` 报告 `enabled=0`，两路都无效。需要手动开启：`och rssi on`（或 `och set rx.rssi-enabled on`）。重新初始化 RF 不改变这个开关，断电后恢复为关闭。
- **数值含义。** 读取 AD9361 symbol RSSI，单位为相对 dB，越大越强，**不是校准过的输入功率**。
- **有效条件。** RSSI 已开启，并且 ENSM 处于 RX 或 FDD 状态。`rx_channel` 只决定哪些通道通过 USB 上传；本板运行 2R2T，两路接收机始终在测量，所以 RX2 不上传时也能读到 RSSI。

```
rx_rssi enabled=1 rx1_valid=1 rx1_db=-73.250 rx2_valid=1 rx2_db=-75.000 rx_channel=1 ensm=3 unit=dB_AD9361_relative calibrated_input_power=0
```

OCH 0.4.7 提供 `och_get_rx_rssi` / `och_set_rx_rssi_enabled`、`och rssi [on|off]` 和 `och get rx.rssi`。旧 SDK 没有 `rssi=1`，因此没有开关：OCH 从 `system_status?` 的 RSSI 行读取，`enabled` 视为 1，开关命令返回不支持。查询和开关都不会暂停 IQ 流。

**波特率。** Zynq PL 的 UART16550（SDK `console.h` 中的 `CONSOLE_BAUD_RATE`）与 CH32 USART1（UHSIF 工程 `Common/usbss_uart1_bridge.h` 中的 `UART1_BRIDGE_BAUD`）之间的控制链路，从 115200 提高到 **3.125 MBd**。两边必须相同，并且必须同时更新：一边新、一边旧时，`och find` 仍能看到设备，但 `och info` 会因 `host_protocol?` 超时而失败。

| 一侧 | 时钟 | 波特率公式 | 上限 |
|---|---|---|---|
| Zynq AXI UART16550 | 100 MHz（`XPAR_AXI_UART16550_0_CLOCK_FREQ_HZ`） | 100 MHz / (16 × 整数除数) | 6.25 MBd |
| CH32H417 USART1 | HCLK 100 MHz（SYSCLK 400 MHz / 4） | HCLK / (16 × USARTDIV)，USARTDIV 带 4 位小数 | 6.25 MBd（手册标称 9 Mbps 需要更高的 HCLK） |

由于 16550 只支持整数除数，两边都能精确实现的速率只有 6.25 MBd / N：6.25M、3.125M、2.083M、1.5625M……SDK 用编译期检查保证这一点。选 3.125 MBd（N=2）而不是 6.25 MBd，是因为 Zynq 控制台轮询接收，16 字节 FIFO 在 3.125 MBd 下约 51 µs 填满，在 6.25 MBd 下只需约 26 µs；而再翻倍每条命令只能多省约 0.1 ms。如果 FIFO 溢出，SDK 会回复 `Error: UART receive overflow`，不会悄悄丢掉命令。CH32 的 8 KiB 接收环形缓冲在 3.125 MBd 下必须每约 26 ms 处理一次，主循环满足这个要求。

实测（USB3，RX 双通道 30.72 MSPS 流运行中）：

| 项目 | 115200 | 3.125 MBd |
|---|---|---|
| 命令往返（`rx_lo_freq?`） | 7.2 ms | 0.6 ms |
| `system_info?`（916 B） | 81 ms | 3.4 ms |
| `rx_rssi?` | — | 1.1 ms |
| `rx_lo_freq=`（同频段） | 11.2 ms | 3.8 ms |
| `och_configure` 流运行中切频 | 11.3 ms | 4.0 ms |
| 跨增益表切频 | 32.7 ms | 25.4 ms（主要是 SPI 写增益表） |

满速双通道：RX 2 通道 61.44 MSPS（12bit，368.6 MB/s）运行 60 秒，开启 RSSI 并每 10 ms 读取一次，共 3,538 次，全部有效，平均 1.26 ms、最长 3.15 ms；吞吐 61.44 MS/s/通道，无间断、无设备或主机溢出、无丢样，与不读 RSSI 时完全一致。

压力测试：连续 4,530 条命令（包括 9.7 KB 的 `help?` 回复），没有出错，也没有 UART 溢出；长回复约 0.3 MB/s，即线路速率上限；期间 RX 保持 30.72 MS/s。
