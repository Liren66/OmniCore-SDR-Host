# 更新记录

各版本新增的功能。固件对应关系见 [固件兼容性](docs/firmware-compatibility.md)，实测数据见 [测试结果](docs/benchmarks.md)。C ABI 自始至终为 1：新增字段都追加在结构末尾或占用原保留位，按 `struct_size` 兼容旧调用方；新增函数要配套含这些导出的库。

## 0.8.0（2026-11-13）

- SoapySDR 插件（`bindings/soapy`，驱动名 `omnicore`）：GNU Radio、SDR++、SDRangel、SoapySDR Python / C++ 可直接使用。只提供源码编译。见 [SoapySDR 插件](docs/soapy.md)。
- MATLAB 工具箱（`bindings/matlab`）：`omnicore.Device`、`RxStream` / `TxStream`、`Receiver` / `Transmitter` System object 与 Simulink；三个平台各一个 zip，Windows 安装包可选安装。见 [MATLAB 工具箱](docs/matlab.md)。
- 两者共用 `bindings/common/` 的参数映射与流逻辑，只调用公开 C 接口（`bindings_use_public_api` 测试保证）。

## 0.7.2（2026-11-11）

- 支持 macOS 12+（通用二进制，libusb 静态链接）；Linux 与 macOS 共用 `src/transport/posix`。
- 核心重构：传输层抽象为 `src/transport/link.hpp`（`core_is_portable` 测试保证核心不含平台代码），新增 FakeLink 离线测试。
- CI 与发布流水线（Windows 安装包、Linux / macOS 归档）。

## 0.7.1（2026-11-08）

- 桥接固件 1.0.1 下 `och info` / `och power` 每次实时读取 USB-C 状态（EP3 OUT 发送 `typec?`）。

## 0.7.0（2026-11-07）

- 多板同步：`och sync` 与 `och_multi_sync()`，菊花链或公共 10 MHz + PPS，含锁定检查、AD9361 MCS 与公共时间装载。需要固件 `sync=1`。见 [多板同步](docs/multi-board-sync.md)。

## 0.6.2（2026-11-07）

- `och info` 恢复 OmniCore SDR 字标；README 改用实机终端截图。

## 0.6.1（2026-11-07）

- `och_event` 末尾追加 `count`：固件合并过载期间的欠流 / GAP 事件时，一条记录代表多个设备事件。

## 0.6.0（2026-11-07）

- 成帧 USB 传输成为唯一格式：每个 16 KiB 块带 16 字节头与 64 位样点时间，溢出丢样数由时间戳差精确给出。需要 FPGA `CH32H417_IP` 1.3；旧固件只能使用 0.5.x。
- 硬件时间戳、PPS 对时、定时 RX、定时 TX 突发与带样点时间的事件。见 [硬件时间戳](docs/timestamps.md)。

## 0.5.0（2026-11-03）

- `och firmware` 通过 USB 升级 CH32 桥接固件（需要桥接固件 1.0.0+）。见 [桥接固件升级](docs/firmware-update.md)。

## 0.4.x（2026-10）

- 0.4.8：仅 PPS 时由 CH32 PA4 DAC 驯服 40 MHz 晶振（`clock.vcxo-status`）。见 [PPS](docs/pps.md)。
- 0.4.7：RX RSSI 查询与开关（`och rssi`）；控制串口提高到 3.125 MBd。
- 0.4.6：快速调谐：只改 LO、RX 增益或 TX 衰减时不暂停流。
- 0.4.5：许可证定为 GPL-3.0-or-later。
- 0.4.4：Linux libusb 后端；BBPLL 无法精确实现的采样率改用最近的整 Hz 速率。
- 0.4.3：USB2 实测档位与 CLI 参考值。
- 0.4.2：分层的终端输出；支持 USB 2.0 High-Speed。
- 0.4.0：2TX2RX 可选 8bit USB IQ。见 [8bit USB IQ](docs/iq8.md)。

## 0.3.x

- 0.3.6：兼容新旧两种 RGB 默认亮度（15% / 25%）的固件。
- 0.3.5：RGB 状态灯、风扇温控与 Zynq 温度。见 [板级控制](docs/board-control.md)。
- 0.3.4：PPS 测频结果作为 ±1 ppm LOCK 显示。
- 0.3.3：PPS 频率监控。
- 0.3.2：ADF4002 外部 10 MHz 参考与 RX / TX 独立外部 LO。见 [参考与外部 LO](docs/reference-lo.md)。
- 0.3.1：RX 直流与 IQ 跟踪校正开关。
- 0.3：运行中改参：设备协调暂停、应用、恢复，流句柄保持不变。

## 0.2.x

- 0.2.1：USB-C 启动电源快照（`och power`）。
- 0.2：AD9361 硬件 FIR 装载与启停、六级时钟链配置。见 [FIR 与时钟](docs/fir.md)。
