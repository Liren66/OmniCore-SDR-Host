# OmniCore Host (OCH)

OmniCore Host 是 **OmniCoreSDR**（AD9361 + Zynq + CH32H417 USB 桥）的主机端软件：C++17 运行库、命令行工具 `och` 和公共 C/C++ SDK。它负责设备发现、射频控制、连续 IQ 收发、RX 数据边界恢复与吞吐测试，可作为 GNU Radio、MATLAB、SDRangel 等上层软件的接入基础。

| 版本 | 平台 | 接口 | 许可证 |
|---|---|---|---|
| **0.7.2**（C ABI 1） | Windows 10/11 x64（libusbK）<br>Linux x86_64（libusb 1.0） | 纯 C ABI + 仅头文件 C++ RAII 包装 | GPL-3.0-or-later |

![OmniCore Host 架构](docs/images/architecture.svg)

> [!IMPORTANT]
> **0.6.x 需要 Zynq 固件 [v2026.11.07](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07) 及以上；推荐 [v2026.11.07.1](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07.1)，过载时欠流 / GAP 事件合并上报，0.6.1 按 `count` 计数。** USB 传输改为成帧格式（每个 16 KiB 块带 16 字节头和 64 位样点时间），并新增硬件时间戳、PPS 对时、定时 RX 与定时 TX。旧固件请继续使用 OCH 0.5.x，两者不能混用，见 [固件兼容性](docs/firmware-compatibility.md)。

> [!NOTE]
> **0.7.1：`och info` / `och power` 每次都能读到 USB-C 电流，需要 CH32 桥接固件 1.0.1（固件 [v2026.11.08](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.08)，`och firmware update OmniCore-Bridge-1.0.1.ocfw`）。** 桥接固件 1.0.0 下只有上电后第一次可读。
>
> **0.7.0 新增多板同步（`och sync`），需要 Zynq 固件 [v2026.11.07.2](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07.2)（SDK_CMD_1.2）。** 其余功能在 v2026.11.07 及以上的固件上照常可用，见 [多板同步](docs/multi-board-sync.md)。

## 目录

1. [功能](#功能)
2. [快速上手](#快速上手)
3. [命令行](#命令行)
4. [Linux 配置](#linux-配置)
5. [测试结果](#测试结果)
6. [C/C++ 开发](#cc-开发)（含 [硬件时间](#硬件时间)）
7. [从源码构建与打包](#从源码构建与打包)
8. [工程目录](#工程目录)
9. [文档索引](#文档索引)

## 功能

| 类别 | 内容 |
|---|---|
| 多板同步 | `och sync`：无外部参考时菊花链共享 10 MHz（REF_OUT → 10M_EXT）并由主板 TRIG 同步；或所有板共用 10 MHz + PPS（原子钟、GPSDO）。含锁定检查、AD9361 多片同步和公共时间装载，需固件 SDK_CMD_1.2，见 [多板同步](docs/multi-board-sync.md) |
| 硬件时间戳 | 64 位样点时间（1 tick = 1 样点）、外部 / 内部 PPS 对时、定时 RX、定时 TX 突发、带样点时间的事件，见 [硬件时间戳](docs/timestamps.md) |
| 设备与射频 | 设备发现、状态查询；RX/TX LO、增益、衰减、带宽、通道配置 |
| IQ 数据流 | 单/双通道 RX、TX 与全双工；文件采集与回放、测试正弦、吞吐统计 |
| 数据完整性 | 每个 16 KiB 块带 16 字节头（序号、有效帧数、样点时间、校验）；溢出丢样数由时间戳差精确给出，以 epoch 和元数据标记间断 |
| USB IQ 格式 | 默认 12bit packed；2TX2RX 可选 8bit signed，降低带宽占用 |
| 桥接固件升级 | `och firmware update` 通过 USB 升级 CH32H417 桥接固件，中断后可重来（需桥接固件 1.0.0+） |
| FIR 与时钟 | AD9361 硬件 FIR 装载与启停、采样率与六级时钟链配置 |
| 采样率就近 | BBPLL 小数分频无法精确实现的速率自动改用最近的整 Hz 速率并提示，如 11.2 MSPS → 11200011 Hz |
| 运行中改参 | 不销毁流句柄，协调暂停 → 应用配置 → 恢复 |
| 校正与参考 | RX 直流 / IQ 跟踪；ADF4002 内/外 10 MHz 参考；RX/TX 独立外部 LO |
| PPS 驯服 | 仅 PPS 时由 CH32 PA4 DAC 驯服 40 MHz 晶振（相位锁定 ±1 µs）；与 10 MHz 同用时仅作频偏校验 |
| 监控与板控 | PPS 频偏监控（±1 ppm LOCK）、RGB 状态灯、按温度调速的风扇 |
| USB 链路 | USB 3.0 SuperSpeed；USB 2.0 High-Speed 适用于低采样率 |
| SoapySDR 插件 | `driver=omnicore`，接入 GNU Radio（gr-soapy）、SDR++、SDRangel、Python 等，见 [SoapySDR 插件](docs/soapy.md) |

## 快速上手

### 1. 安装

**Windows**：运行 `OmniCoreHost-0.7.2-win64-setup.exe`（[Releases](https://github.com/Liren66/OmniCore-SDR-Host/releases)），默认安装到 `C:\Program Files\OmniCore Host` 并加入 PATH。安装包含 `och.exe`、运行库、头文件、MinGW/MSVC 导入库、CMake 包配置、示例与文档，以及驱动助手：

```powershell
och-driver check      # 只检查，不修改系统
och-driver install    # 为 VID 1A86 / PID 5537 安装 libusbK（需 UAC）
```

**Linux**：

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config libusb-1.0-0-dev
bash tools/build-linux.sh
sudo cmake --install build/linux --prefix /opt/omnicore/0.7.2
sudo ln -sfn /opt/omnicore/0.7.2/bin/och /usr/local/bin/och
sudo install -m 0644 packaging/linux/60-omnicore.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

运行无需 sudo。USB 权限、目录布局与性能参数见 [Linux 配置](#linux-配置)；完整说明见 [Windows 安装](packaging/windows/INSTALL_WINDOWS.txt) / [Linux 安装](docs/INSTALL_LINUX.md)。

### 2. 连接设备

```powershell
och version        # 软件版本与 ABI
och find           # 枚举设备
och info           # 身份、固件版本、USB 链路、RF 能力、时间基准、电源
och selftest       # 数字接口自检（RX PRBS / TX 回环）
```

<p align="center"><img src="docs/images/cli-info.png" width="640" alt="och info：OmniCore SDR 标题、设备身份、三类固件版本、RF 能力、时间基准与电源摘要"></p>
<p align="center"><sub>实机截图：OCH 0.7.1，USB 3.0 SuperSpeed，Zynq <code>SDK_CMD_1.2_20261107_SYNC</code>，CH32 桥接 1.0.1，USB-C 3000 mA</sub></p>

### 3. 收发 IQ

```powershell
# 双通道接收 5 秒：LO 1 GHz、每通道 15.36 MSPS，保存为 sc16
och rx --file rx.sc16 --format sc16 --channels 1,2 --lo-frequency 1G --sample-rate 15.36M --seconds 5

# 回放上述文件，循环发射 5 秒，TX 衰减 60 dB
och tx --file rx.sc16 --format sc16 --channels 1,2 --lo-frequency 1G --sample-rate 15.36M --loop --seconds 5 --attenuation 60

# 发射 1 MHz 基带测试音
och tx --tone 1M --amplitude 0.25 --channels 1 --lo-frequency 1G --sample-rate 15.36M --seconds 5 --attenuation 60

# 全双工吞吐测试（不读写文件）
och bench --rx --tx --channels 1,2 --sample-rate 30.72M --seconds 10
```

> [!NOTE]
> 发射命令会产生射频输出，请接负载或衰减后的测试回路。`Ctrl+C` 可随时停止；已有采集文件默认不覆盖（需 `--overwrite`）。

### 设备固件要求

安装包只含主机软件，不自动烧写板级固件。0.6.x 需要 [OmniCore-SDR-Firmware](https://github.com/Liren66/OmniCore-SDR-Firmware/releases) v2026.11.07 及以上的 `BOOT.bin`（FPGA `CH32H417_IP` 1.3、`SDK_CMD_1.1`）；打开设备时检查 `transport=framed16k`，不满足则拒绝连接。CH32 桥接固件可用 `och firmware update FILE.ocfw` 手动通过 USB 升级，见 [桥接固件升级](docs/firmware-update.md)。设备还需 SDK 命令封装 v1 与 CH32 诊断协议 v4；扩展功能按 `och get system.info` 返回的能力字段启用（如 `rx_correction=1`、`external_lo=1`、`pps_monitor=1`），缺少时只拒绝对应功能。TX FIR 插值 ×2 / ×4 需要包含 TX 系数回读修复的 SDK 固件，旧固件只能使用插值 1。详见 [固件兼容性](docs/firmware-compatibility.md)。

## 命令行

### 升级桥接固件

CH32 桥接固件 1.0.0 及以上可直接通过 USB 升级，升级包从 [OmniCore-SDR-Firmware Releases](https://github.com/Liren66/OmniCore-SDR-Firmware/releases) 下载：

```bash
och firmware info                                  # 当前版本与模式
och firmware update OmniCore-Bridge-1.0.0.ocfw     # 约 5 秒，中断后可重新执行
```

`och firmware info` 显示 `legacy` 或 `Bootloader: none` 时，需先用 WCH-Link 烧录一次 `Merge.bin`。
完整步骤见 [Firmware README：升级 CH32 桥接固件](https://github.com/Liren66/OmniCore-SDR-Firmware#升级-ch32-桥接固件)，命令与恢复说明见 [桥接固件升级](docs/firmware-update.md)。

### 命令一览

| 类别 | 命令 |
|---|---|
| 设备 | `find` · `info` · `power` · `version` · `selftest` |
| 控制 | `commands` · `get NAME` · `set NAME VALUE` · `action NAME` · `config [RF 选项]` |
| FIR / 时钟 | `fir status` · `fir rx\|tx load FILE` · `fir rx\|tx\|both on\|off` · `clock status` · `clock sample-rate HZ` |
| 数据流 | `rx --file PATH` · `tx --tone HZ` · `tx --file PATH [--loop]` · `bench --rx [--tx]` |
| 多板同步 | `sync --devices M,S1,...` · `sync --mode common` · `sync status` · `sync off` |
| 桥接固件 | `firmware [info]` · `firmware check FILE` · `firmware update FILE` · `firmware boot` · `firmware enter` |

全局选项：`--serial` 选择设备，`--json` 输出机器可读结果，`--no-color` 关闭颜色。数值支持 `k`/`M`/`G` 后缀，规范命令统一使用 Hz 和 dB。

<details>
<summary><b>常用控制示例</b></summary>

```powershell
och set rx.lo-frequency 2.4G
och set tx1.attenuation 10
och config --sample-rate 30.72M --lo-frequency 1G --bandwidth 18M --gain 30

och set rx.dc-correction on          # RX 直流跟踪
och set clock.ref-source external    # 外部 10 MHz 参考（J3）
och set rx.lo-source external        # 外部 LO（RX: J11，TX: J10，输入为 2 倍 RF 频率）
och set pps.input-enabled on         # PPS 输入（J6）；无 10 MHz 时驯服晶振
och get pps.status
och get clock.vcxo-status            # 调谐模式与 CH32 环路状态

och fir rx load filter.ftr && och fir rx on
och get core.temperature
och rssi on                          # RSSI 默认关闭，先开启
och rssi --count 0 --interval 0.2   # RX1/RX2 RSSI（相对 dB）
och set fan.enabled on
```

全部命令、范围与单位见 `och commands --json` 与 [命令目录](docs/commands.md)。

</details>

### 终端输出

交互终端中按“标题 → 分组 → 字段”分层显示，带颜色、状态标签和实时进度条；重定向到文件时自动切换为纯 ASCII（≤ 78 列），`--json` 则只输出 JSON。事件每条一行，带硬件时间的事件附样点时间；结束时按方向给出吞吐与故障计数：

<table>
<tr>
<td width="50%" valign="top"><img src="docs/images/cli-bench-running.png" alt="och bench 2T2R 12bit 44 MSPS 运行中：会话、RF 配置与实时进度"></td>
<td width="50%" valign="top"><img src="docs/images/cli-bench-summary.png" alt="och bench 结束：吞吐汇总表与 RX / TX 分组统计，零故障"></td>
</tr>
<tr>
<td align="center"><sub>运行中：会话信息、RF 配置和实时进度</sub></td>
<td align="center"><sub>结束时：吞吐汇总表与分组统计</sub></td>
</tr>
</table>

以上为实机截图：Windows Terminal，OCH 0.6.2，固件 v2026.11.07.1，2T2R 12bit 44 MSPS（吞吐与 0.7.1 相同）。

退出码：`0` 完成 · `1` 设备/传输错误 · `2` 参数错误 · `3` `bench --fail-fast` 提前结束 · `130` Ctrl+C 中断。性能是否达标请检查 JSON 的 `stability.pass`，而非仅看退出码。

### IQ 文件格式

| 格式 | 每通道每样本 | 说明 |
|---|---:|---|
| `packed12` | 3 字节 | USB 原生 12bit |
| `sc16` | 4 字节 | int16 I/Q，小端；`rx`/`tx` 默认 |
| `fc32` | 8 字节 | float32 I/Q，归一化到 ±1 |
| `sc8` | 2 字节 | int8 I/Q，需要 `--iq-bits 8` |

双通道按 `I1,Q1,I2,Q2` 交织，文件无容器头。采集时旁边生成 `.metadata.jsonl`，记录配置、每个 epoch 的文件位置、首样点硬件时间（`time_ticks`）、精确丢样数与最终统计——**溢出前后的数据不能当作连续采样拼接**，但可按 `time_ticks` 放回同一时间轴。

![RX 数据边界](docs/images/rx-boundaries.svg)

完整参数见 [命令行参考](docs/cli.md)。

## Linux 配置

Linux 版提供与 Windows 相同的 `och` 命令行、C ABI 1 和 C++ 包装，底层使用 libusb 1.0 异步传输。已在 Ubuntu 24.04.3 x86_64 + USB3 上验证；其他发行版建议从源码构建，ARM64 与 Linux 下的 USB2 性能尚未测试。

| 项目 | 配置 |
|---|---|
| 依赖 | `build-essential cmake ninja-build pkg-config libusb-1.0-0-dev`；运行时只需 `libusb-1.0-0` |
| USB 权限 | 安装 `packaging/linux/60-omnicore.rules`（只匹配 `1a86:5537`）。桌面会话经 uaccess 授权；SSH 用户执行 `sudo usermod -aG plugdev "$USER"` 后重新登录，并重新插拔设备 |
| 安装布局 | 推荐 `/opt/omnicore/<版本>`，再把 `bin/och` 软链接到 `/usr/local/bin`。程序使用相对 RPATH，无需 `LD_LIBRARY_PATH`；需保持 `bin/` 与 `lib/` 的相对位置 |
| 发布归档 | 解压 `OmniCoreHost-<版本>-linux-x86_64.tar.gz` 后整体复制到安装目录；udev 规则同时位于 `share/omnicore/60-omnicore.rules` |
| USB 队列 | 默认每方向 32 × 196608 B，在系统默认 `usbfs_memory_mb=16` 下即可满速。仅在增大 `--queue` / `--block` 后出现 `LIBUSB_ERROR_NO_MEM` 时才需调高该内核参数，程序不会自动修改 |
| 主机缓冲 | `--buffer-mib` 调整用户态环形缓冲（默认 64 MiB），与内核 USB 队列预算无关 |
| 构建参数 | `tools/build-linux.sh` 支持 `OCH_JOBS`（并发数）、`OCH_BUILD_DIR`、`OCH_INSTALL_DIR`；脚本优先使用 `/usr/bin` 下的工具，避免被其他 SDK 的旧版 CMake 覆盖 |

```bash
och info                     # 若报 LIBUSB_ERROR_ACCESS，检查 udev 规则与 plugdev 组
ldd "$(readlink -f /usr/local/bin/och)"   # 按真实路径检查动态库解析
och bench --rx --tx --iq-bits 8 --channels 1,2 --sample-rate 61.44M \
  --seconds 60 --amplitude 0 --attenuation 89.75 --json   # 检查 stability.pass
```

## 测试结果

### 0.6.x 成帧传输 · Windows · USB3

固件 v2026.11.07.1（`CH32H417_IP` 1.3，位流与 v2026.11.07 相同），OCH 0.6.1，Windows 11，USB 3.0，2026-11-07。`och bench` 零错误的最高采样率：先按 0.5 MS/s 细扫（每档 10 秒 × 2–3 次），再在最高档做 300 秒长测。USB MB/s 含每块 16 B 头，理论值不含块头。

| 配置 | 采样率 / 通道 | 测试 | RX / TX MB/s | 理论 MB/s | 错误 |
|---|---:|---|---:|---:|---:|
| 1 RX | 61.44 MSPS | 5 秒 | 184.1 / — | 184.3 | 0 |
| 2 RX | 61.44 MSPS | 60 秒 | 368.5 / — | 368.6 | 0 |
| 1 RX + 1 TX | 61.44 MSPS | 5 秒 | 183.9 / 183.8 | 184.3 | 0 |
| 1 RX + 2 TX | **58 MSPS** | 300 秒 | 174.2 / 348.3 | 174.0 / 348.0 | 0 |
| 2 RX + 1 TX | **58.5 MSPS** | 300 秒 × 2 | 351.3 / 175.7 | 351.0 / 175.5 | 0 |
| 2 RX + 2 TX · 12bit | **44 MSPS** | 300 秒 × 2 | 264.2 / 264.2 | 264.0 | 0 |
| 2 RX + 2 TX · 8bit | 61.44 MSPS | 60 秒 | 245.2 / 245.1 | 245.8 | 0 |

上限来自 FPGA 与 CH32 之间的 UHSIF 总线：RX 与 TX 共享约 532 MB/s，总流量 522–528 MB/s 时稳定。高一档的结果：2T2R 12bit 44.5 MSPS、1T2R 59 MSPS 出现 RX 溢出 / TX 欠流；2T1R 58.5 MSPS 10 秒通过，但 300 秒内有 1 次 RX 溢出和 3 次 TX 欠流。0.6.0 发布时的档位搜索（61.44 / 56 / 50 / 46 / 42）跳过了这些中间速率，因此当时记为 56 与 42 MSPS。块头开销（16 / 16384 ≈ 0.1%）对带宽没有可测影响。`tools/bench_sweep.py` 已改为细扫档位，可复现这张表。

下面各节为 0.4.x（起始头模式、固件 `SDK_CMD_1.0_20261024_IQ8`）的历史测量，Linux 与 USB2 尚未在 0.6.x 成帧传输下复测。

吞吐使用 `och bench` 测量，取整个流生命周期的平均值（含启停开销），MB/s 为十进制。理论 wire 速率 = 采样率 × 通道数 × 每样本字节（12bit 为 3，8bit 为 2）。“错误”指主机丢样、USB 传输错误、设备溢出 / 欠流、事件丢失与 TX 断供的合计。

### 测试环境

| | Windows | Linux |
|---|---|---|
| 主机 | ThinkPad E14 Gen 7，Core Ultra 7 255H，32 GB | AMD Ryzen 5 5500U，6 核 12 线程 |
| 系统 | Windows 11 25H2（build 26200） | Ubuntu 24.04.3，Linux 7.0 x86_64 |
| USB 驱动 | libusbK 3.1.0.0 | libusb 1.0.27（usbfs） |
| 编译器 | MinGW-w64 GCC，Release | GCC 13.3，Release |
| 板卡固件 | `SDK_CMD_1.0_20261024_IQ8` | `SDK_CMD_1.0_20261024_IQ8` |
| 缓冲配置 | 每方向 32 × 196608 B USB 请求，64 MiB 环形缓冲 | 同左，`usbfs_memory_mb=16`（系统默认） |

### USB 3.0 · Windows

![USB3 吞吐实测（Windows）](docs/images/throughput.svg)

| 配置 | USB IQ | 时长 | RX / TX MB/s | 理论 MB/s | 比例 RX / TX | 错误 |
|---|---:|---:|---:|---:|---:|---:|
| 2 RX · 61.44 MSPS | 12bit | 5 秒 | 367.2 / — | 368.6 | 99.6% / — | 0 |
| 1 RX · 61.44 MSPS | 12bit | 5 秒 | 183.6 / — | 184.3 | 99.6% / — | 0 |
| 2 TX · 61.44 MSPS | 12bit | 5 秒 | — / 362.0 | 368.6 | — / 98.2% | 0 |
| 2 RX + 2 TX · 30.72 MSPS | 12bit | 5 秒 | 183.4 / 180.9 | 184.3 | 99.5% / 98.1% | 0 |
| 1 RX + 1 TX · 61.44 MSPS | 12bit | 5 秒 | 183.8 / 180.7 | 184.3 | 99.7% / 98.1% | 0 |
| 2 RX + 2 TX · 50 MSPS | 8bit | 5 秒 | 199.2 / 196.3 | 200.0 | 99.6% / 98.1% | 0 |
| 2 RX + 2 TX · 61.44 MSPS | 8bit | 300 秒 | 245.7 / 245.7 | 245.8 | 100.0% / 100.0% | 0 |

### USB 3.0 · Linux

![USB3 吞吐实测（Linux）](docs/images/linux-throughput.svg)

| 配置 | USB IQ | 时长 | RX / TX MB/s | 理论 MB/s | 比例 RX / TX | 错误 |
|---|---:|---:|---:|---:|---:|---:|
| 2 RX · 61.44 MSPS | 12bit | 60 秒 | 368.6 / — | 368.6 | 99.98% / — | 0 |
| 1 RX · 61.44 MSPS | 12bit | 60 秒 | 184.3 / — | 184.3 | 99.98% / — | 0 |
| 2 TX · 61.44 MSPS | 12bit | 60 秒 | — / 368.1 | 368.6 | — / 99.85% | 0 |
| 2 RX + 2 TX · 30.72 MSPS | 12bit | 60 秒 | 184.3 / 184.0 | 184.3 | 99.98% / 99.85% | 0 |
| 1 RX + 1 TX · 61.44 MSPS | 12bit | 60 秒 | 184.3 / 184.0 | 184.3 | 99.98% / 99.85% | 0 |
| 2 RX + 2 TX · 50 MSPS | 8bit | 300 秒 | 200.0 / 199.9 | 200.0 | 100.0% / 99.97% | 0 |
| 2 RX + 2 TX · 61.44 MSPS | 8bit | 300 秒 | 245.8 / 245.7 | 245.8 | 100.0% / 99.97% | 0 |

Linux 测试的 TX 发送零 IQ、衰减 89.75 dB；所有档位 `stability.pass=true`，未调整内核 USB 内存配额、实时调度或 CPU 电源策略。

### USB 2.0 High-Speed · Windows

USB2 下 RX 与 TX 共享 480 Mbit/s 链路预算，下表为按 0.5 MSPS 步进得到的稳定采样率（每通道），每项连续运行 300 秒：

![USB2 吞吐实测](docs/images/usb2-throughput.svg)

| 配置 | USB IQ | 时长 | RX / TX MB/s | 理论 MB/s | 比例 RX / TX | 错误 |
|---|---:|---:|---:|---:|---:|---:|
| 1 RX · **14** MSPS | 12bit | 300 秒 | 42.0 / — | 42.0 | 100.0% / — | 0 |
| 2 RX · **7** MSPS | 12bit | 300 秒 | 42.0 / — | 42.0 | 100.0% / — | 0 |
| 1 TX · **14** MSPS | 12bit | 300 秒 | — / 42.0 | 42.0 | — / 99.97% | 0 |
| 2 TX · **6.5** MSPS | 12bit | 300 秒 | — / 39.0 | 39.0 | — / 99.97% | 0 |
| 1 RX + 1 TX · **7** MSPS | 12bit | 300 秒 | 21.0 / 21.0 | 21.0 | 100.0% / 99.97% | 0 |
| 2 RX + 2 TX · **3.5** MSPS | 12bit | 300 秒 | 21.0 / 21.0 | 21.0 | 100.0% / 99.97% | 0 |
| 2 RX + 2 TX · **5** MSPS | 8bit | 300 秒 | 20.0 / 20.0 | 20.0 | 100.0% / 99.97% | 0 |

USB2 测试的 TX 为 1 MHz 正弦、幅度 0.25、衰减 60 dB。启动后 1 秒内允许 RX 溢出并完成重同步（1 RX、1 RX + 1 TX 与 8bit 全双工各出现 1–2 次），之后零新增错误。USB2 需要支持 USBHS 的 CH32 固件，详见 [USB2 稳定传输](docs/usb2.md)。

### 功能测试

| 项目 | 平台 | 结果 |
|---|---|---|
| 硬件时间戳（0.6.0） | Windows | `hw_timestamp_test` 6 项通过：内部 PPS 间隔恰为采样率（61.44 / 30.72 / 7.68 MSPS）、PPS 装载精确、RX 时间戳连续、定时 RX 首样点精确、定时 TX 经数据口回环延迟逐突发恒定、迟到突发不发射 |
| 数字接口自检 | Windows / Linux | RX PRBS 与 TX 数字回环通过 |
| CLI 功能检查 | Linux | 20 / 20 通过：设备信息、时钟、FIR、温度、SC16 / FC32 / packed12 采集与元数据、独占访问、Ctrl+C 退出与重开、文件保护、循环回放、非法参数 |
| 运行中改参 | Windows / Linux | RX、零 IQ TX、双向及 FIR / 时钟操作后原流恢复收发，以新 epoch 标记间断 |
| 参考与外部 LO | Windows | 参考 / LO 11 项检查与 3 次运行中切换通过；ADF4002 D5 自检闪烁序列确认 |
| RX 校正 | Windows | DC / IQ 跟踪 18 项检查与 6 次运行中切换通过 |
| PPS 监控 | Windows | 真实 PPS 下 30.72 MSPS 读数 122880000–122880001 Hz，频偏收敛至 0–0.008 ppm |
| PPS 驯服晶振 | Windows | 4 种调谐模式与 300 次参考切换握手零丢失；空闲、RX 2×2 与 IQ8 全双工各 2 分钟全程锁定，相位 ≤ 158 ns，频偏 ≤ 0.016 ppm |
| SDK 集成 | Linux | 独立 C / C++ CMake 工程链接发布包并运行；移动安装目录后仍可运行 |
| 桥接固件 USB 升级 | Windows | USB3：连续 15 次成功，约 5 秒/次；USB2（480 Mbit/s）：连续 6 次成功，约 10 秒/次（Bootloader 先尝试 USB3）。两种链路下写入中途结束进程后设备停在 Bootloader，重新升级恢复；升级后 SDK 控制与 IQ 收发正常 |

> [!NOTE]
> 以上结果来自所列主机，USB 吞吐与主机控制器、端口和线材有关，更换环境后建议用 `och bench --json` 复核 `stability.pass`。测试只覆盖数据链路与控制功能，不包含 RF 功率、EVM、灵敏度等射频指标。

## C/C++ 开发

| 头文件 | 说明 |
|---|---|
| `include/omnicore/och.h` | 纯 C ABI，不跨 DLL 传递 STL / 异常，便于 MSVC、MinGW、C#、Python、MATLAB MEX 调用 |
| `include/omnicore/och.hpp` | 仅头文件的 C++17 RAII 包装 |

```cmake
find_package(OmniCoreHost 0.6 CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE OmniCore::omnicore)
```

```cpp
#include <omnicore/och.hpp>

och::device dev;                         // 唯一设备；多台时传序列号
auto cfg = och::default_config();
cfg.sample_rate = 30720000;
cfg.rx_mask = 3;                         // RX1 + RX2
dev.configure(cfg);

och::stream rx(dev, och::stream_args(OCH_RX));
rx.start();
// rx.recv(...) 或 och::rx_lease 零拷贝读取；元数据中的 epoch / flags 标记数据间断
```

运行中改参由同进程管理线程发起，流句柄保持不变，RX 以新 epoch 标记间断。只改频率、增益或衰减时不暂停流，单次切频约 11 ms（见 [快速调谐](docs/api.md#快速调谐046)）：

![运行中改参](docs/images/reconfigure.svg)

### 硬件时间

![硬件时间：PPS 对时、定时 RX 与定时 TX](docs/images/timestamps.svg)

```c
och_set_time_source(dev, OCH_TIME_SOURCE_INTERNAL);     // 或 EXTERNAL（J6 PPS）
och_set_time(dev, 0, 1);                                // 下一个 PPS 沿把时间置 0

och_stream_args a; och_default_stream_args(&a, OCH_RX); // 定时 RX：t = T 起接收 N 个样点
a.start_flags = OCH_RX_TIMED; a.num_samples = N;
a.start_time_lo = (uint32_t)T; a.start_time_hi = (uint32_t)(T >> 32);

och_tx_metadata md = {sizeof md, OCH_TX_HAS_TIME | OCH_TX_START_OF_BURST | OCH_TX_END_OF_BURST, T2};
och_send_burst(tx, samples, n, &md, &accepted, 1000);   // 在样点时间 T2 发射
```

RX 元数据的 `time_ticks` 是每批第一个样点的时间；突发完成、迟到、欠流以带 `hw_time` 的事件上报。详见 [硬件时间戳](docs/timestamps.md)。

示例：[接收](examples/receive.cpp) · [发射](examples/transmit.cpp) · [运行中改参](examples/reconfigure.cpp) · [8bit 全双工](examples/duplex_iq8.cpp)。接口与生命周期详见 [API 参考](docs/api.md)。

从源码构建时示例默认一起编译（`-DOCH_BUILD_EXAMPLES=OFF` 可关闭），可执行文件在构建目录的 `examples/` 下（Windows 与 `omnicore.dll` 同目录）。已安装的 OCH 也可以单独编译示例：

```bash
cmake -S <安装目录>/examples -B build-examples -DCMAKE_PREFIX_PATH=<安装目录>
cmake --build build-examples
```

## 从源码构建与打包

需要 CMake 3.20+、Ninja 与 C++17 x64 工具链（Windows 使用 MSYS2 MinGW-w64，默认 `C:\msys64\mingw64\bin`）。

```powershell
# Windows：编译并安装到 build/stage
powershell -File tools/build.ps1 -Configuration Release

# 生成安装包 dist/OmniCoreHost-<版本>-win64-setup.exe 及 .sha256
powershell -NoProfile -File packaging/windows/build-package.ps1 -StageDir build/stage
```

```bash
# Linux：编译并安装到 dist/OmniCoreHost-linux
bash tools/build-linux.sh
```

打包脚本会校验 stage 内容与版本号，加入驱动助手、libusbK 与许可证，生成 MSVC 导入库（需 VS Build Tools 的 `lib.exe`），并写入 `PAYLOAD_SHA256.txt`。Inno Setup 6.2.2 已随工程提供，详见 [安装包构建说明](packaging/windows/BUILD_INSTALLER.txt)。

## 工程目录

```text
OmniCoreHost/
├── include/omnicore/   公共 C ABI (och.h) 与 C++ 包装 (och.hpp)
├── apps/
│   └── och/            och 命令行
├── src/
│   ├── capi/           公共 C 接口入口（extern "C"，按模块分文件）
│   ├── core/           设备、配置事务、流线程、射频与板控
│   ├── dsp/            IQ 编解码
│   ├── protocol/       16 KiB 成帧块解析与生成
│   └── transport/      libusbK (Windows) / libusb (Linux) 传输层
├── bindings/
│   └── soapy/          SoapySDR 插件（driver=omnicore）与实机验收脚本（hwtest/）
├── examples/           C++ 示例、FTR 示例、GNU Radio 流图（soapy/）
├── tests/              成帧解析单元测试、硬件时间戳板上测试
├── docs/               使用与开发文档、插图
├── cmake/              CMake 包配置模板
├── tools/              构建脚本、吞吐扫描（bench_sweep.py）
├── packaging/
│   ├── windows/        Inno 安装包脚本、MSVC 导入库生成、驱动助手源码（driver/）
│   └── linux/          udev 规则
└── third_party/        libusbK / libwdi 及其许可证、Inno Setup 编译器
```

## 文档索引

| 使用 | 开发 | 专题 |
|---|---|---|
| [命令行参考](docs/cli.md) | [API 参考](docs/api.md) | [FIR 与时钟](docs/fir.md) |
| [命令目录](docs/commands.md) | [架构与性能](docs/architecture.md) | [参考时钟与外部 LO](docs/reference-lo.md) |
| [Windows 安装](packaging/windows/INSTALL_WINDOWS.txt) | [设备协议](docs/protocol.md) | [PPS 监控](docs/pps.md) |
| [Linux 安装](docs/INSTALL_LINUX.md) | [固件兼容性](docs/firmware-compatibility.md) | [板级控制](docs/board-control.md) |
| [排错与能力边界](docs/troubleshooting.md) | | [8bit USB IQ](docs/iq8.md) · [USB2](docs/usb2.md) |
| [桥接固件升级](docs/firmware-update.md) | [硬件时间戳](docs/timestamps.md) | [多板同步](docs/multi-board-sync.md) |
| [SoapySDR 插件](docs/soapy.md) | | |

## 许可证

Copyright (C) 2026 OmniCoreSDR contributors

本工程源码采用 [GNU 通用公共许可证 第 3 版](LICENSE)（`SPDX-License-Identifier: GPL-3.0-or-later`）：你可以按自由软件基金会发布的 GPL 第 3 版，或（由你选择）任何更新的版本，重新分发和/或修改本程序。本程序按“原样”提供，不附带任何担保。分发副本或衍生作品时须使用相同许可证并提供对应源代码。提交到本项目的贡献同样按 GPL-3.0-or-later 授权。libusbK、libwdi 等第三方组件沿用各自许可证，见 `third_party/driver/NOTICES.txt`。
