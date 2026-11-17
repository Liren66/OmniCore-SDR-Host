# 固件与协议依赖

主机安装器只安装 Windows 程序、主机 C/C++ 开发接口和目标数据口的 libusbK 驱动，不含板级 SDK 源码副本、ELF、BOOT.bin 或 FPGA bitstream，不自动烧写 CH32、SDK、FPGA 或启动 Flash。把软件装到新电脑后仍须连接具有匹配固件的 OmniCoreSDR。

## 版本对应

OCH 0.6.0 起只支持成帧 USB 传输（每个 16 KiB 块带 16 B 头和样点时间），需要 Zynq 固件 [v2026.11.07](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07) 及以上。打开设备时若 `system_info?` 不含 `transport=framed16k`，`och_open` 返回 `OCH_UNSUPPORTED`，提示加载 FPGA `CH32H417_IP` 1.3 与 `SDK_CMD_1.1`。

当前版本 OCH 0.8.0 推荐使用固件 [v2026.11.08](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.08)（Zynq `SDK_CMD_1.2_20261107_SYNC` + CH32 桥接 1.0.1）。0.7.2、0.8.0 没有新的固件要求：0.7.2 增加 macOS 支持并重构核心，0.8.0 增加 SoapySDR 插件与 MATLAB 工具箱，两者都只调用公开 C 接口。

| OCH | Zynq 固件（BOOT.bin） | FPGA `CH32H417_IP` | SDK 标识 | CH32 桥接 |
|---|---|---|---|---|
| **0.8.0** | 同 0.7.1，推荐 v2026.11.08 | 1.3（`0x01030001`） | `SDK_CMD_1.2_20261107_SYNC` | **1.0.1**（1.0.0 仍可用） |
| 0.7.2 | 同 0.7.1 | 1.3（`0x01030001`） | `SDK_CMD_1.2_20261107_SYNC` | 1.0.1（1.0.0 仍可用） |
| 0.7.1 | 同 0.7.0；每次都能读到 USB-C 电流需 CH32 桥接 1.0.1（[v2026.11.08](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.08)） | 1.3（`0x01030001`） | `SDK_CMD_1.2_20261107_SYNC` | **1.0.1**（1.0.0 仍可用，USB-C 电流只在上电后第一次可读） |
| 0.7.0 | v2026.11.07 及以上；`och sync` 需要 v2026.11.07.2（SDK_CMD_1.2，FPGA 带 OC_SYNC_AXI 1.0） | 1.3（`0x01030001`） | `SDK_CMD_1.2_20261107_SYNC` | 1.0.0（不变） |
| 0.6.1 | v2026.11.07 及以上，推荐 v2026.11.07.1 | 1.3（`0x01030001`） | `SDK_CMD_1.1_20261107_EVTCOUNT`（v2026.11.07.1） | 1.0.0（不变） |
| 0.6.0 | v2026.11.07 及以上 | 1.3（`0x01030001`） | `SDK_CMD_1.1_20261107_FRAMED` | 1.0.0（不变） |
| 0.5.x | v2026.11.03 及以前 | 1.1 | `SDK_CMD_1.0_*` | 1.0.0 / legacy |

0.6.1 与 v2026.11.07 也能配合使用，只是过载时固件逐条上报事件、不带 `count`（每条记 1）；v2026.11.07.1 合并过载事件并带 `count=`，见 [硬件时间戳](timestamps.md)。

两边必须配套：0.6.0 不能连接旧固件，0.5.x 也不能使用新固件收发 IQ（新固件不再发送 16 KiB 递增起始头）。CH32 桥接固件对数据内容透明，两种组合都可用 1.0.0。

0.5.0 起，CH32 桥接固件 1.0.0 及以上可由用户手动执行 `och firmware update FILE.ocfw` 通过 USB 升级，详见 [桥接固件升级](firmware-update.md)。`och firmware info` 显示桥接固件版本；1.0.0 之前的桥接固件显示 legacy。

## 能力检查

OCH 按固件回报的能力字段启用功能，不按固件名字判断：FIR/时钟等扩展曾沿用旧的固件名字（如 `SDK_CMD_1.0_20261023`），名字相同不代表功能相同。缺少某项能力时，只有对应的 API / 命令返回 `OCH_UNSUPPORTED`，其余功能照常；能力检查在暂停数据流之前完成，不支持的请求不会打断已有的流。

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

旧 SDK 即使支持原始串口命令，也可能缺少命令封装协议或成帧传输，不能直接假定兼容；版本不满足时 OCH 明确失败，避免把超时或残留回复误认为新命令已成功。各命令的格式见 [设备协议](protocol.md)。

板级 SDK、FPGA 与 CH32 工程在 [OmniCore-SDR-Firmware](https://github.com/Liren66/OmniCore-SDR-Firmware) 仓库，独立于 OmniCore Host。主机安装包不升级 Zynq / FPGA 固件；RAM 中运行的新 ELF 不等于断电后仍会启动该版本，长期使用请按固件仓库的流程部署启动镜像。自定义 FIR / 时钟、RX 校正开关等设置只保存在运行中 SDK 的 RAM，断电或重新下载程序后恢复默认。

打开设备会停止旧 USB 会话并清理缓存，不要让其他上位机、调试脚本和 OCH 同时操作同一个数据口。只需检查 Windows 驱动时用 `och-driver check`，只需枚举时用 `och find`。

## 需要多端同时更新的功能

| 功能 | 固件要求 | 不满足时 |
|---|---|---|
| PPS 驯服 40 MHz（PA4 调谐硬件） | SDK `vcxo=1 pps_adjustment=1` + 带 PA4 握手的 CH32 桥接固件 | 旧桥接固件把 PA4 当 LED 推挽驱动且不回应握手，新 SDK 因此拒绝打开 ADF4002 电荷泵（`error=-116`），外部 10 MHz 设置失败而不会与 PA4 冲突；旧桥接固件也不能用于仅 PPS 模式。`och_get_vcxo_status` 在旧 SDK 上返回 `OCH_UNSUPPORTED`。见 [PPS](pps.md) |
| 板级 RGB、风扇、Zynq 温度 | FPGA 带 BOARD_CTRL_AXI，SDK `board_rgb=1 fan_control=1 core_temp=1` | 对应命令不支持。新旧 SDK 的默认亮度（15% / 25%）都能识别，见 [板级控制](board-control.md) |
| USB 2.0 High-Speed | 支持 USBHS 的 CH32 桥接固件 | 旧桥接固件缺少 USBHS 控制端点和 0xD0 诊断 / 流清理；只支持 512 B 最大包长的 High-Speed，64 B 的 Full-Speed 一律拒绝 |
| RX RSSI 开关 | SDK `rssi=1`（`rssi_enable`、`rx_rssi?`） | 旧 SDK 没有开关：OCH 从 `system_status?` 的 RSSI 行读取（约 190 ms，新 SDK 约 1.1 ms），`enabled` 视为 1，开关命令返回不支持。含义见 [命令行：RX RSSI](cli.md#rx-rssi) |
| 控制串口 3.125 MBd | Zynq SDK 与 CH32 桥接固件同时更新 | 一边新、一边旧时，`och find` 仍能看到设备，但 `och info` 因 `host_protocol?` 超时而失败。见 [设备协议：控制串口](protocol.md#控制串口) |

USB2 下 `och info` 精确显示 **480 Mbit/s（High-Speed, USB 2.0）**；其中的 61.44 MSPS 是射频采样率上限，不是 USB2 可持续速率，USB2 档位见 [测试结果](benchmarks.md#usb-20-high-speed)。

## TX FIR 插值 ×2 / ×4

AD9361 驱动写入 TX FIR 系数时，会把 `0x002[1:0]` 临时设为目标插值；旧 SDK 的 `fir_verify_coeff`（`console_commands/sdr_fir.c`）却在写入后的当前模式下回读。TX FIR 关闭（旁路）时，×2 / ×4 滤波器的回读与写入不一致，SDK 判定失败并回滚，返回 `ERR fir_or_clock_apply code=-1 rollback=verified`。因此旧固件无法在关闭状态装载 TX 插值滤波器（含 `och fir tx load`），已开启的 ×2 / ×4 TX FIR 也无法关闭。

修复后的 SDK 在回读 TX 系数时进入 ALERT 状态，把 `0x002[1:0]` 临时设为配置的插值，读完恢复原值；系数仍被逐个核对。修复版已实测：TX ×2 / ×4 在关闭状态装载、开启与关闭均成功，RX / TX 各种因子组合下的采样率就近选择与全双工吞吐测试通过。RX 抽取不受此问题影响。该修复只在板级 SDK 中，主机程序无需改动；固件版本字符串未变，请以能否装载 TX 插值滤波器来确认所用固件。
