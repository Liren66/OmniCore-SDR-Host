# 文档插图

README 与 `docs/` 中使用的正式插图，保留 SVG 图表与 PNG 终端图片。

| 文件 | 内容 | 数据来源 |
|---|---|---|
| `architecture.svg` | 软件分层、控制面 / 数据面、USB 链路与板卡组成 | `docs/architecture.md` |
| `rx-boundaries.svg` | RX 成帧块头时间、溢出 GAP、epoch 与 metadata 记录 | `src/core/stream.cpp` 的元数据标志 |
| `timestamps.svg` | PPS 对时、定时 RX、定时 TX 突发与事件的时间关系 | `docs/timestamps.md`、FPGA `CH32H417_IP` 1.3 |
| `reconfigure.svg` | 运行中改参的暂停、应用、恢复流程 | `docs/architecture.md` |
| `wire-format.svg` | packed12 / signed8 字节布局、应用格式、16 KiB 成帧块 | `docs/iq8.md`、编解码实现 |
| `clock-tree.svg` | 外部 10 MHz、ADF4002、AD9361 时钟链、外部 LO、PPS 监控 | 设备 `clock status` / `adf4002_status` 回读 |
| `throughput.svg` | 实测 USB 吞吐与理论 wire 速率 | 2026-10-25 CLI 实板测试与 8bit 61.44 MSPS 长时间测试（OCH 0.4.2） |
| `linux-throughput.svg` | Linux 版 USB3 实测吞吐与理论 wire 速率 | 2026-10-25 Ubuntu 24.04.3 实板测试（OCH 0.4.3） |
| `usb2-throughput.svg` | USB2 七种通过配置的实测吞吐与理论 wire 速率 | 2026-10-25 USB2 300 秒测试（OCH 0.4.3） |
| `cli-usb2-info.png` | USB2 设备信息，与 USB3 使用相同布局 | 测量记录中的设备信息，经 CLI 显示代码绘制 |
| `cli-usb2-bench.png` | 8bit 双通道全双工 5 MSPS、300 秒测试输出 | 300 秒测试记录，经 CLI 显示代码绘制 |
| `cli-*.png` | 终端输出截图 | 见下文 |

`cli-info.png` 是 Windows Terminal 的实机截图（OCH 0.7.1，Zynq `SDK_CMD_1.2_20261107_SYNC`，CH32 桥接 1.0.1，USB 5 Gbit/s，2026-11-08）。`cli-bench-running.png`、`cli-bench-summary.png` 也是实机截图（OCH 0.6.2，固件 v2026.11.07.1，USB 5 Gbit/s，2026-11-07；2T2R 12bit 44 MSPS，TX 衰减 89.75 dB）。其余终端图片按 CLI 显示代码（`src/cli/presentation.hpp`）与 Windows Terminal 默认配色绘制，使用 0.4.x 时期（固件 `SDK_CMD_1.0_20261024_IQ8`）的实测数据，较长的输出以 `...` 标明省略部分。
