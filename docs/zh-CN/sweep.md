# 扫频瀑布图示例

[`examples/sweep.cpp`](../../examples/sweep.cpp) 在一条持续运行的 RX 流上逐步切换 RX LO，把每一步的 FFT 拼成整段频谱（每遍扫描一行瀑布图），并记录每次切频的耗时。[`examples/sweep_plot.py`](../../examples/sweep_plot.py) 负责画图。

```bash
sweep --start 70M --stop 6G --sweeps 20 --out scan      # 全频段 20 遍
python3 sweep_plot.py scan                              # 频谱 + 瀑布图 + 耗时直方图 + 切频功率轨迹
python3 sweep_plot.py scan --follow                     # 与 sweep 同时运行，实时刷新（配合 --sweeps 0）
```

## 演示界面

[`examples/sweep_live.py`](../../examples/sweep_live.py) 启动 `sweep --stream`，并在本机提供一个网页（[`sweep_live.html`](../../examples/sweep_live.html)），只用 Python 标准库：

```bash
python3 examples/sweep_live.py --sweep build/examples/sweep --open   # 打开 http://127.0.0.1:8765
```

- **顶部卡片**（界面为英文）：每遍耗时、扫描速度（GHz/s、步/秒）、同频段切频中位数（并换算成理论上限次数/秒）、跨增益表切频、每步总耗时、数据完整性（RX 间断、丢样、重采）。
- **频谱**：实时曲线和最大保持（橙色），淡蓝色光带标出当前 LO 正在扫的那一段，虚线是 1.3 / 4 GHz 增益表分界。鼠标悬停读出频率和电平。
- **瀑布图**：最近 150 遍，自动色阶，也可以手动调。
- **交互**：用预设（全频段、FM、蜂窝、2.4G、5G WiFi、L 波段）选频段，也可以在频谱或瀑布图上拖动框选来放大，双击回到全频段。点击 “Strongest signals”（最强信号）列表中的一项，会放大到该信号附近 ±20 MHz。采样率、增益、每段保留比例、平均次数改完后按 “Apply”（应用）生效，此时会重启 sweep。
- 整个范围落在一步之内时，不需要切频，相当于一台固定调谐的实时频谱仪：服务端把刷新限制在每秒 25 帧，速度卡片显示 “Fixed tuning”（固定调谐）。
- 一旦出现数据间断，日志里会记下它发生在第几遍、第几步和对应的 LO 频率。
- 演示运行期间设备一直被 sweep 占用。按 Ctrl+C（或发 SIGTERM）退出时，sweep 会正常停流并关闭设备。

实测：全频段每遍约 1.0 s（161 步，每步约 5.2 ms，`--no-bracket`）；2.39–2.5 GHz 只有 3 步，每遍约 17 ms，能直接看到 WiFi 突发。

默认参数：61.44 MSPS，FFT 2048 点（分辨率 30 kHz）× 平均 16 次，USB 传输 64 KiB（`--transfer-kib`），每步保留中间 60% 的 FFT 点（步进 36.84 MHz，全频段 161 步），手动增益 30 dB，LO 中心处的直流泄漏用两侧 5 个点插值抹平。`sweep --help` 列出全部选项。整段步数凑不齐时，网格会整体平移回 70 MHz–6 GHz 以内。

## 怎么确定哪些样本属于新频率

快速调谐不停流，所以下一块数据上的 `RECONFIGURED` 标记只是近似边界：`och_configure` 返回时，USB 队列里可能还有整整一队列旧数据（库默认 32 × 192 KiB，30.72 MSPS 单通道约 68 ms）。示例改用硬件时间来定边界：

1. 切频前读一次设备时间 `before`（`och_get_time`，即 `time_status?`）；
2. `och_configure` 只改 `rx_frequency`，走快速调谐路径；
3. 返回后再读一次设备时间 `after`。SDK 按顺序处理命令，所以 LO 写入和 PLL 锁定等待一定发生在 `after` 之前；
4. 只使用时间戳不早于 `after + --guard-us`（默认 50 µs，用来覆盖 AD9361 数字滤波器的延迟）的样本，与 USB 队列深度无关。

`after - before` 是用样点时间量出的切频区间上限，其中包含两次命令往返。`--no-bracket` 跳过第一次时间读取，每步约省 1.8 ms，但这样就没有区间和功率轨迹。

## 输出文件

| 文件 | 内容 |
|---|---|
| `PREFIX.json` | 频率轴（`first_bin_hz`、`bin_hz`、`row_bins`）、参数；结束时写入 `summary` |
| `PREFIX.waterfall.f32` | 每遍扫描一行 float32 dBFS，行长 `row_bins` |
| `PREFIX.steps.jsonl` | 每步一行：`configure_us`（主机侧切频耗时）、`time_reads_us`、`capture_us`、`total_us`、`bracket_us`、`gain_table`、`discarded`、`restarts` |
| `PREFIX.trace.jsonl` | 前 `--trace-sweeps` 遍中每一步的功率轨迹：从 `before` 开始，每 64 个样点一个点 |

`--stream` 不写文件，改为在标准输出逐行输出 JSON：`meta` 一行，每步一行 `step`，每遍一行 `row`（其中 `db` 是 float32 小端数据的 base64 编码），结束时一行 `summary`。人读的信息改走标准错误。

dBFS 以满幅复正弦为 0 dBFS，不是校准过的输入功率。

## 实测（2026-11-14）

macOS，USB 3.0，Zynq `SDK_CMD_1.2_20261107_SYNC`，CH32 桥接 1.0.1；70 MHz–6 GHz，单通道。各项为中位数，同频段步。

传输大小与时间读取（30.72 MSPS，258 步 × 23.04 MHz）：

| 配置 | 切频 | 读时间 | 采集 | 每步 | 每遍 | RX 间断 |
|---|---:|---:|---:|---:|---:|---:|
| 192 KiB 传输（库默认），读两次时间 | 3.04 ms | 3.55 ms | 3.36 ms | 10.0 ms | 2.76 s | 0（20 遍） |
| 64 KiB，读两次时间 | 3.03 ms | 3.53 ms | 0.47 ms | 7.1 ms | — | 0（5 遍） |
| 16 KiB，读两次时间 | 3.03 ms | 3.52 ms | 0.25 ms | 6.8 ms | — | **1 次溢出**（5 遍） |
| 64 KiB，`--no-bracket` | 3.02 ms | 1.76 ms | — | 5.3 ms | 约 1.45 s | 0（10 遍） |

采样率（全部 64 KiB 传输，0 次 RX 间断）：

| 采样率 / FFT / 保留 | 步进 | 步数 | 每步 | 每遍 | 每点分辨率 |
|---|---:|---:|---:|---:|---:|
| 30.72 MSPS / 1024 / 0.75，`--no-bracket` | 23.04 MHz | 258 | 5.36 ms | 1.55 s | 30 kHz |
| 61.44 MSPS / 1024 / 0.75，`--no-bracket` | 46.08 MHz | 129 | 5.20 ms | 0.78 s | 60 kHz |
| 61.44 MSPS / 2048 / 0.6，`--no-bracket` | 36.84 MHz | 161 | 5.23 ms | 1.00 s | 30 kHz |
| **61.44 MSPS / 2048 / 0.6（现默认，读两次时间）** | 36.84 MHz | 161 | 7.12 ms | 1.31 s（20 遍） | 30 kHz |

每步耗时几乎不随采样率变化：主要花在切频上，采集只占约 0.5 ms。所以采样率翻倍，步数减半，每遍时间也差不多减半。代价是单通道 USB 流量从 92 MB/s 升到 184 MB/s，仍远低于实测上限。保持每点分辨率不变（FFT 加到 2048）时，每步采集时长不变。

- 跨增益表的步中位数 29.6 ms，每遍约 4 步，加起来约占每遍的 0.12 s。
- 样本要等整个 USB 传输填满才能到达主机：192 KiB 在 30.72 MSPS 下就是约 2.1 ms。所以示例默认用 64 KiB。16 KiB 已出现设备溢出，不建议使用。
- `time_status?` 一次约 1.76 ms，比 `rx_lo_freq?`（约 0.6 ms）慢得多，是除切频之外最大的一项开销。
- 每段两侧边缘的噪底比中间高，在 1.3–4 GHz 段最多高约 7.6 dB。把模拟带宽从 27.6 MHz 收窄到 20 MHz 几乎没有改善（+7.1 dB），所以不是模拟通带外信号的混叠。更可能是 ADC（245.76 MHz）经三级半带抽取后，噪声在 Nyquist 附近折叠回来。对比结果（3 遍中位数）：

  | 设置 | 步进 | 每遍 | 边缘抬升（<1.3G / 1.3–4G / >4G） | 中心残留 |
  |---|---:|---:|---|---|
  | `--usable 0.75 --dc-bins 2`（原默认） | 23.04 MHz | 1.55 s | +1.8 / +7.6 / +4.8 dB | 最多 +1.7 dB |
  | `--usable 0.6 --dc-bins 5` | 18.42 MHz | 1.91 s | +0.2 / +2.1 / +0.9 dB | 最多 +0.9 dB |

  61.44 MSPS 下边缘表现相同：保留 0.75 时为 +0.9 / +6.2 / +4.3 dB，0.6 时为 +0.6 / +1.9 / +0.9 dB，0.85 时为 +1.9 / +8.9 / +6.6 dB。因此示例默认用 61.44 MSPS 配 `--usable 0.6`、`--dc-bins 5`，比原来 30.72 MSPS 配 0.75 既更快，边缘也更干净。彻底解决需要一组真正的低通 FIR，配合 ×2 抽取（ADC 491.52 MHz）。注意：FIR 只在抽取之前起作用，×1 的 FIR 去不掉已经折叠进来的噪声。

## 读结果时注意

- `gain_table=1` 的步跨越了 1.3 GHz / 4 GHz（或在其 ±1 MHz 内），SDK 要重写增益表，耗时约 25 ms；每遍的第一步从 6 GHz 跳回起点，也属于这一类。直方图把它们单独列出。
- 每步耗时 = 切频 + 两次时间读取 + 采集（默认 2048 × 16 = 32768 个样点 ≈ 0.53 ms）+ FFT。目前单步瓶颈在固件执行 `rx_lo_freq=`（AD9361 驱动每次切频都重新校准 VCO 并等待锁定）。要做到每秒上万次跳频，需要固件支持 AD9361 fastlock 和设备端定时跳频，当前固件尚未提供。
- `restarts` 非零表示某一步采集过程中出现了时间不连续（溢出或丢样），该步已从头重新采集；`summary` 里的 `rx_gaps`、`host_dropped_samples`、`device_overflows` 应为 0。
- 宽带扫描时，RX 直流 / IQ 跟踪环路来不及在每一步收敛，可能残留中心尖峰，可用 `--dc-bins` 调整抹平宽度。
