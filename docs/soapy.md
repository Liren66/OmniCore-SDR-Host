# SoapySDR 插件（GNU Radio、SDR++、SDRangel、Python）

`bindings/soapy` 是 OmniCoreSDR 的 SoapySDR 模块，驱动名 `omnicore`。装好之后，所有支持 SoapySDR 的软件都能直接使用这块板：GNU Radio（`gr-soapy`，3.9 起自带；GNU Radio 4 也自带 Soapy 块）、SDR++、SDRangel、CubicSDR、Pothos，以及 SoapySDR 的 Python / C++ 接口。

插件只用公共 C ABI（`och.h`），不依赖 OCH 内部代码。设计与映射细节见仓库根目录的 `SOAPY_PLAN.md`。

| 状态 | 内容 |
|---|---|
| 已实机验证（macOS） | 设备发现与参数、RX 单/双通道（CS16/CF32）、溢出与时间戳、定时与定长 RX、GNU Radio `rx_time` 标签 |
| 已实现、待实机验证 | TX 连续与突发发送、`readStreamStatus`、全双工、8bit 模式 |
| 不支持 | 多板同步、FIR 装载、时钟链手动配置（用 `och` 命令行） |

## 编译与安装

插件是 C++ 模块，必须和机器上的 SoapySDR 用**同一个 SoapySDR 版本、同一种编译器**编译，所以目前以源码方式提供。需要 SoapySDR 0.8 或更新版本。

**Linux**（以 Debian/Ubuntu 为例）：

```bash
sudo apt-get install libsoapysdr-dev soapysdr-tools
cmake -S . -B build -DOCH_BUILD_SOAPY=ON
cmake --build build -j
sudo cmake --install build --prefix /usr/local
```

插件装到 `/usr/local/lib/SoapySDR/modules<版本>/libomnicoreSupport.so`。如果系统的 SoapySDR 不在 `/usr/local` 下，也不会去这个目录找插件，这时把这个目录加进环境变量：

```bash
export SOAPY_SDR_PLUGIN_PATH=/usr/local/lib/SoapySDR/modules0.8
```

**macOS**（Homebrew）：

```bash
brew install soapysdr libusb pkg-config
cmake -S . -B build -DOCH_BUILD_SOAPY=ON
cmake --build build -j
cmake --install build --prefix "$(brew --prefix)"
```

Homebrew 升级 SoapySDR 的次版本后（例如 0.8 → 0.9），需要重新编译插件。

**只编插件**（已经安装了 OCH，例如 Linux 包或 Windows 安装包）：

```bash
cmake -S bindings/soapy -B build-soapy -DCMAKE_PREFIX_PATH=/opt/omnicore/0.7.2
cmake --build build-soapy -j
```

**Windows**（未验证）：Windows 上常见的 SoapySDR 来自 radioconda / conda-forge 或 PothosSDR，都是 MSVC 编译的，所以插件也要用 MSVC 编：在装好 SoapySDR 的 conda 环境里，`CMAKE_PREFIX_PATH` 同时指向 OCH 安装目录（安装包已带 MSVC 导入库）和 `%CONDA_PREFIX%\Library`，再把生成的 `omnicoreSupport.dll` 放进 `%CONDA_PREFIX%\Library\lib\SoapySDR\modules0.8`。OCH 的 `bin` 目录要在 PATH 上（安装包默认已加）。

检查是否装好：

```bash
SoapySDRUtil --info                       # Available factories 里应有 omnicore
SoapySDRUtil --find="driver=omnicore"
SoapySDRUtil --probe="driver=omnicore"
```

## 设备参数

设备串写成 `driver=omnicore,key=value,...`，GNU Radio 里填在 Device 参数和 Device Arguments 中。

| 参数 | 取值 | 说明 |
|---|---|---|
| `serial` | 序列号 | 多块板时选择其中一块；不写时只接一块板才能打开 |
| `rx_mask`、`tx_mask` | 0–3 | 打开时就设好两个方向的通道（1 = 通道 0，2 = 通道 1，3 = 两个）。**GNU Radio 全双工必须写**，见下文 |
| `iq_bits` | 12 / 8 | USB 样点位宽；8 要求两个方向都是双通道 |
| `clock_source` | `internal` / `external` | 10 MHz 参考 |
| `time_source` | `none` / `external` / `internal` / `sync` | PPS 时间源 |

OCH 对设备有跨进程独占锁：插件打开设备期间，`och` 命令行不能再打开同一块板。同一个程序里的多个 Soapy 块（例如 GNU Radio 的 source 和 sink）共用同一个设备实例，不受影响。

## 射频参数

| Soapy 接口 | 范围 | 共享关系 |
|---|---|---|
| 采样率 | 2.5 – 61.44 MSPS | RX、TX 和两个通道共用一个；无法精确实现的速率改用最近的整 Hz 速率并在日志中提示 |
| 频率（元件 `RF`） | 70 MHz – 6 GHz | 同一方向两个通道共用一个 LO |
| 带宽 | 200 kHz – 56 MHz | 同一方向两个通道共用 |
| RX 增益（元件 `PGA`） | −3 dB 到 73 dB（RX 本振 ≤ 1.3 GHz）/ 71 dB（1.3–4 GHz）/ 62 dB（> 4 GHz），步进 1 | 每通道独立；`getGainRange` 按当前 RX 频率返回上限，设得更高会被压到上限，改频率时已设的增益超过新频段上限也会自动降下来（日志里有提示） |
| TX 增益（元件 `PGA`） | 0 … 89.75 dB，步进 0.25 | 每通道独立；增益 = 89.75 − 衰减（与 PlutoSDR 的约定一致），**0 为最小功率** |
| RX AGC | 开 / 关 | 开启时用 `rx_agc_mode` 设置的模式（默认 slow） |
| 直流 / IQ 自动校正 | 开 / 关 | RX 两通道共用；需要固件支持 |

采样率下限 2.5 MSPS 是保守值（不开 FIR 抽取时 AD9361 最低约 2.083 MSPS），实机验证后会放宽。

只改频率、增益时数据流不中断。改采样率或带宽时，流会暂停后恢复，期间读写可能返回超时，属于正常现象。

## 数据格式

| 格式 | 说明 |
|---|---|
| `CS16`（原生，满量程 32768） | 12bit 样点左对齐到 16 位 |
| `CF32` | 归一化浮点 |
| `CS8`（仅 `iq_bits=8`，原生） | 8bit 有符号 |

不提供 Soapy 的 `CS12`：设备线上 12bit 打包的字节顺序和 Soapy 的定义不同。

流参数（`setupStream` 的 args，GNU Radio 的 Stream Arguments）：`transfer_bytes`（USB 传输大小，默认 196608）、`usb_queue_depth`（默认 32）、`host_buffer_bytes`（主机缓冲，默认 16 MiB）。

## 设置与传感器

设置（`writeSetting` / `readSetting`，GNU Radio 里也可以通过 `cmd` 消息端口的 `setting` 命令修改）：

| 键 | 取值 | 说明 |
|---|---|---|
| `iq_bits` | 12 / 8 | 只能在没有数据流时修改 |
| `rx_agc_mode` | manual / fast / slow / hybrid | |
| `rx_lo_source`、`tx_lo_source` | internal / external | 外部 LO 输入频率为 RF 的 2 倍 |
| `pps_enable` | true / false | PPS 频率监控（同时启动 VCXO 驯服环路） |
| `rssi_enable` | true / false | 打开后 `rssi` 传感器才有读数 |
| `rgb_enable`、`fan_enable` | true / false | 状态灯、风扇 |

传感器：`temperature`（Zynq 温度）、`ref_locked`、`pps_locked`、`vcxo_locked`、`time_valid`、`usb_speed`，以及每个 RX 通道的 `rssi`（相对 dB，未校准）。固件不支持的项读作 `false` 或 `nan`。

## 硬件时间

- 时钟源：`internal` / `external`（10 MHz）。时间源：`none` / `external` / `internal` / `sync`。
- `getHardwareTime("")` 读当前时间，`getHardwareTime("pps")` 读最近一个 PPS 沿的时间；`setHardwareTime(t)` 立即设置，`setHardwareTime(t, "pps")` 在下一个 PPS 沿生效。
- 每次 `readStream` 都带 `HAS_TIME` 和第一个样点的时间。
- **改采样率会让硬件时间失效**：计数继续累加，但按新速率换算，时间会跳变。改完采样率后要重新 `setHardwareTime`。

## 接收行为

- 丢样（设备溢出、主机丢样，或时间戳不连续）时，`readStream` 先单独返回一次 `SOAPY_SDR_OVERFLOW`，下一次调用再交出数据和它的时间。GNU Radio 收到溢出后会重新打 `rx_time` 标签，所以下游的时间始终正确。
- 定时 RX 迟到时同样先返回 `SOAPY_SDR_TIME_ERROR`。
- `activateStream(stream, HAS_TIME, t)` 在时间 t 开始接收；`activateStream(stream, END_BURST, 0, n)` 只收 n 个样点，最后一次读取带 `END_BURST`。

## 发送行为

- **启动时机**：`activateStream` 只做准备，真正开始发送要等写入的数据够一个 USB 传输（默认每通道 16368 个样点），或写完一个带 `END_BURST` 的突发。数据不足一个传输、又没写 `END_BURST` 就停止时，这些数据不会发出，日志里有警告。
- **突发**：Soapy 没有“突发开始”标志，上一个突发以 `END_BURST` 结束后的下一次写入，就是新突发的开始。`HAS_TIME` 的写入总是开始一个新突发，在指定时间发出。
- **只写 `END_BURST` 不带数据**（`numElems = 0`）：插件补 1 个零样点来结束突发。
- **`readStreamStatus`**：突发发送完成返回 0 并带 `END_BURST` 和时间；设备欠流返回 `SOAPY_SDR_UNDERFLOW`；定时突发迟到返回 `SOAPY_SDR_TIME_ERROR`。突发之间的正常空闲不算欠流。
- **短突发**：刚激活后第一个突发很短也没问题（实测 1 个样点到 1.6 万个样点，定时与不定时都能发出并收到确认），gr-soapy 的长度标签突发可以直接用。

## GNU Radio

用 Soapy Custom Source / Sink（或 Python 的 `gnuradio.soapy.source` / `sink`），Device 填 `driver=omnicore`。

- **全双工**：source 和 sink 必须在 Device Arguments 里写**相同**的 `rx_mask` 和 `tx_mask`，例如单通道收发写 `rx_mask=1,tx_mask=1`，双通道写 `rx_mask=3,tx_mask=3`。原因是 GNU Radio 先启动一个块再启动另一个；第二个块启动时第一个已经在跑，这时 core 不允许修改通道配置。
- **时间标签**：source 在启动、改频率、改采样率和溢出之后打 `rx_time`、`rx_rate`、`rx_freq` 标签。
- **发送**：sink 支持长度标签突发（`set_length_tag_name`），但 gr-soapy **不支持定时发送**（不读 `tx_time`/`tx_sob`/`tx_eob` 标签）。需要定时发送时，直接用 SoapySDR 的 Python / C++ 接口。
- **运行中改参数**：改 RX 本振、增益不会丢数据。改 TX 本振、带宽或采样率时，设备会短暂暂停传输，全双工下 RX 会丢几十到一两百毫秒的数据。source 会把每段空档都上报，并重新打 `rx_time` 标签。
- **启停**：整个流图停止再启动没有问题。全双工时只停一个方向再单独重启会失败（core 要求两个方向都停下才能重开），下一版 core 会放宽。

示例流图在 `examples/soapy/`。

## Python

```python
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

sdr = SoapySDR.Device("driver=omnicore")       # 用字符串；见下方说明
sdr.setSampleRate(SOAPY_SDR_RX, 0, 10e6)
sdr.setFrequency(SOAPY_SDR_RX, 0, 1e9)
rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
buf = np.zeros(sdr.getStreamMTU(rx), np.complex64)
sdr.activateStream(rx)
sr = sdr.readStream(rx, [buf], len(buf))
print(sr.ret, sr.flags, sr.timeNs)
sdr.deactivateStream(rx)
sdr.closeStream(rx)
```

SoapySDR 0.8 的 Python 绑定用 dict 传参数（`SoapySDR.Device(dict(driver="omnicore"))`）会报 `make() no match`，用字符串或 `SoapySDR.SoapySDRKwargs` 就正常。

更完整的用法见 `bindings/soapy/hwtest/` 下的验收脚本。

## 排错

| 现象 | 处理 |
|---|---|
| `--info` 里没有 omnicore | 插件不在 SoapySDR 的模块目录：用 `SOAPY_SDR_PLUGIN_PATH` 指过去，或检查 SoapySDR 版本是否与编译时一致 |
| `--find` 找不到设备 | 先用 `och find` 确认；Linux 检查 udev 规则，Windows 检查 `och-driver check` |
| 打开失败，提示设备忙 | 有其他进程（如 `och` 命令行）正在使用这块板 |
| GNU Radio 全双工启动报 `needs rx_mask=...` | 在 source 和 sink 的 Device Arguments 里写相同的 `rx_mask`、`tx_mask` |
| 双通道 61.44 MSPS 偶尔溢出 | 先用 `och bench` 测同样的速率；macOS 上这一档有设备端溢出，与插件无关 |
| 改采样率后时间跳变 | 正常现象，重新 `setHardwareTime` |
| gr-soapy 偶尔报 `Soapy sink error: TIMEOUT` | 改采样率、带宽时，设备会暂停传输 0.05–0.7 s，超过 100 ms 就会出现几条，属正常现象。如果不改参数也持续出现，看日志里的 `OmniCore TX: write timed out ...`：`started=0` 表示还没开始发送；`running=0` 表示流已停止；`host_queue` 一直满且 `underflows` 不增长，说明设备没在取数据。请把这几行日志连同当时的操作一起反馈 |
