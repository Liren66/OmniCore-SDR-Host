# 设备控制与数据协议参考

本文记录 OmniCore Host 使用的设备协议，供维护控制后端和开发适配器时参考。常规应用应调用 [C/C++ API](api.md)，使用者应优先选择 [规范命令](commands.md) 和 [FIR/时钟命令](fir.md)。本工程与安装包不包含 ZYNQ SDK 源码副本、ELF、BOOT.bin 或 FPGA bitstream，也不自动下载板上固件。

## 能力与端点

不能仅按固件名字判断扩展是否存在。通过 `host_protocol?` 确认 `host_protocol=1`，通过 `system_info?` 确认 `transport=framed16k`（0.6.0 起必需；旧固件的 `header_on=rx_start_and_overflow_recovery` 不再接受）；硬件时间 API 还要求 `hw_time=1`，FIR 和安全时钟配置分别要求 `fir_upload=1`、`clock_config=1`。

RX 校正扩展要求 `system_info?` 含 `rx_correction=1`。`rx_dc_correction?` / `rx_dc_correction=0|1` 同时控制 AD9361 RF 与基带直流跟踪，`rx_iq_correction?` / `rx_iq_correction=0|1` 控制 RX 正交跟踪，均作用于两个物理接收通道。回应的同名标量为 0/1，后面可附加状态字段。SDK setter 要求流已停止，OCH typed API 负责协调现有 RX/TX 会话；CH32 控制/数据协议不变。

0 只关闭跟踪更新，不清除已有校正系数；IQ=0 时硬件 phase/gain correction 仍开启。此接口不表示完全旁路校正或重新执行 TX IQ 校准。

| USB 端点 | 用途 |
|---|---|
| 0x01 OUT | TX 成帧块：16 B 头（HAS_TIME/SOB/EOB、序号、样点时间）+ packed12 或 2TX2RX signed8 IQ |
| 0x81 IN | RX 成帧块：16 B 头（标志、序号、有效帧数、首样点时间）+ packed12/signed8 IQ |
| 0x02 OUT | CH32/ZYNQ 控制命令文本 |
| 0x82 IN | 命令响应与异步 EVT |
| 0x83 IN | 可选的现有 CH32 调试日志；电源查询读取启动快照 |

USB 请求完成边界不是文本行或 IQ 会话边界，一行可以跨请求，一次请求也可以带多行。EP83 信息的非实时限制见 [电源信息](cli.md#usb-c-电源信息)。

隔离版 0.4.0 的 8bit 扩展使用 `iq_format?` / `iq_format=12|8`，能力为 `iq_format=1 iq8_2t2r=1`。默认 12bit，只有 RX/TX mask 均为 3 才能选择 8bit。按 I1,Q1,I2,Q2 顺序传输原 12bit 值的高 8 位，TX 恢复低 4 位零；每时间帧 4 字节。SDK 内部块可能包含两个时间帧，不能用内部块宽替代应用帧宽。格式切换要求双向全停，通常运行可分别启停。CH32 固件不改。详见 [8bit 协议与兼容](iq8.md)。

## 命令封装 v1

主机发送一条 LF 结束的 ASCII 命令；控制台也接受 CR/CRLF，PL UART 输出使用 CRLF。包含封装且不含换行的输入行最长 127 字符，普通命令体允许 ASCII 可打印字符和水平 TAB，禁止 NUL、内嵌换行、非 ASCII 字节或嵌套封装。

~~~text
@<id> <original command>
~~~

id 为 1..4294967295 的十进制 uint32，不带符号或进制前缀；前导零允许，响应使用规范十进制。id 后至少一个空格或 TAB，命令体不能为空，其首个非空白字符不能是 `@`。

~~~text
@42 rx_samp_freq?
RSP id=42 begin
rx_samp_freq=30720000
RSP id=42 end result=0
~~~

每设备只能有一个未完成命令。只有匹配 id 的 begin/end 完成该事务；其他 id 不能完成它，缺 begin 的匹配 end 属于协议错误。重复记录不得改变已完成结果。响应体保留原 SDK 格式，主机把响应行标准化为 LF，累计响应和未结束控制行均限制为 32768 字节。

`result` 是原回调的有符号返回值，通常 0 表示分派成功、-1 表示失败。某些旧回调通过正文打印错误而仍返回 0，因此主机也将 `ERR`、`Error:`、`Invalid` 等既有错误正文视为失败。语法错误封装不会分派，可能只输出 `Invalid host envelope: ...`，没有 RSP；主机必须在发送前验证输入。

不能用“若干毫秒没有新字节”判断成功。必须收到完整事务结束记录，否则报告超时/失败。超时后控制会话视为不可继续使用，应等待设备完成响应，再关闭并重新打开，避免把迟到响应用于下一条命令。

## 异步事件

`EVT ` 开头的行是异步事件，可在命令前、响应中间或响应后到达。控制线程把它送入有界事件队列，不附加到响应体，也不据此完成命令；队列未及时消费时 `event_drops` 可增加。

FPGA 的定时事件带样点时间，由 SDK 按以下格式上报，OCH 映射为对应的 `och_event.type`，并填入 `hw_time`、`code` 和 `block`：

~~~text
EVT tseq=<n> type=<name> src=fpga_time code=<n> block=<seq> time=<ticks> lost_before=<0|1>
EVT tseq=<n> type=time_invalidated src=sdk old_rate=<Hz> new_rate=<Hz>
~~~

| SDK type | code | OCH 事件 |
|---|---|---|
| `tx_burst_ack` | 0x01 | `OCH_EVENT_TX_BURST_ACK` |
| `tx_underflow` | 0x02 | `OCH_EVENT_TX_UNDERFLOW` |
| `tx_late` | 0x03 | `OCH_EVENT_TX_LATE` |
| `tx_seq_error` / `tx_header_error` | 0x04 / 0x05 | `OCH_EVENT_TX_ERROR` |
| `rx_overflow_gap` | 0x10 | `OCH_EVENT_RX_GAP` |
| `rx_late_command` / `rx_command_ignored` | 0x11 / 0x12 | `OCH_EVENT_RX_LATE` |
| `time_invalidated` | — | `OCH_EVENT_TIME_INVALIDATED` |

## 成帧块格式

![16 KiB 块格式](images/wire-format.svg)

IQ 端点上的每个 16384 B 块 = 16 B 头 + 16368 B 载荷。块头为 4 个小端 u32：

~~~text
h0 = 0x4F43_0000 | flags<<8 | check     字节序：check, flags, 'C', 'O'
h1 = seq<<16 | valid                    valid = 本块有效帧数，其后补零
h2 = time[31:0]，h3 = time[63:32]       本块第一个样点的时间，1 tick = 1 个样点
check = 字节 1..15 的 XOR
~~~

| 方向 | flags |
|---|---|
| RX | bit0 TIME_VALID，bit1 SOB，bit2 GAP（此块之前丢过样），bit3 EOB，bit4 LATE，bit5 PAD |
| TX | bit0 HAS_TIME，bit1 SOB，bit3 EOB；`valid=0` 的块被丢弃 |

| wire 格式 | 每帧 | 每块帧数 |
|---|---:|---:|
| 12bit 单通道 | 3 B | 5456 |
| 12bit 双通道 | 6 B | 2728 |
| 8bit 双通道 | 4 B | 4092 |

USB 请求完成边界总是 16 KiB 的整数倍，因此块头总在请求内对齐；核心仍按字节流解析，遇到校验失败会重新搜索块头。

时间相关的 SDK 命令（`time_now?/=`、`time_next_pps=`、`time_last_pps?`、`time_source=`、`time_status?`、`rx_timed=`、`rx_stream=`、`rx_stream_stop!`）由 OCH 的时间 API 调用，详见 [硬件时间戳](timestamps.md) 和 Firmware 仓库的 `docs/sdk-commands.md`。

现有控制台的 UART 接收有有限软件缓冲，错误/溢出时丢弃已排队和部分解析命令，到后续行边界重新同步，并可能输出：

~~~text
Error: UART receive overflow/error; queued and partial commands discarded.
~~~

这不支持无限流水发送，主机仍须等待每次 RSP 完成。

## CH32 诊断与跨会话清理

主机使用现有诊断协议 v4 的 vendor IN 请求：`bmRequestType=0xC0`、`bRequest=0xD0`。`wValue=0` 读取诊断，前两个 uint32 为 magic `0x53444731` 和版本 4；`wValue=2` 请求流缓冲清理。

清理仅在 RX/TX 都停止时执行，并排空残留 RX 数据。因此同一方向 stop 后若要重新开始，需要先停另一方向；核心不会为重开一边而清掉另一边正在使用的数据。此能力是板上既有协议，不表示主机安装器会升级 CH32。

## FIR 分块事务

以下命令仍放在 `@id` 封装里；扩展字段均为十进制整数，不能使用浮点、指数或十六进制。主机要串行管理整个事务，不能让不同调用方交叉上传。

~~~text
rx_fir_begin=<token>,<taps>,<factor>,<gain_dB>
tx_fir_begin=<token>,<taps>,<factor>,<gain_dB>
fir_data=<token>,<offset>,<c0>[,<c1>...<c7>]
fir_commit=<token>,<enable>,<crc32>
fir_abort=<token>
~~~

token 为非零 uint32，RX/TX 共用一个暂存槽。空闲时有效 begin 会替换遗留暂存状态，原 token 随即无效。begin/data 只修改 RAM；系数从 offset=0 开始连续上传，每行 1..8 个 -32768..32767 的 signed16 整数。无效块不改变状态。commit 要求已收到全部 taps、token 匹配、enable 为 0/1、CRC32 匹配。

CRC 为反射 IEEE CRC32，多项式 `0xEDB88320`，初值和最终异或均为 `0xFFFFFFFF`。只序列化声明的系数，每个 int16 二补码、小端低字节在前，不包含元数据或填充；ASCII 字节 `12345678` 对应的 CRC 为 `0x9AE0DAAF`。

taps 为 16..128 且是 16 的倍数，factor 为 1/2/4；TX factor=1 最多 64 taps。RX gain 为 -12/-6/0/6 dB，TX gain 为 -6/0 dB。同方向两个物理通道使用同组配置，实际 tap 上限还受时钟预算影响。不合法组合必须失败，不能静默将采样率改为 1/1.5 MSPS。

成功提交消耗 token；被拒绝或失败的 FIR commit 保留 token，允许显式 retry/abort。成功提交后再 abort 会得到 token mismatch，不能把清理错误替换原错误。

### FIR 查询与启停

~~~text
rx_fir_config?
rx_fir_config=1 taps=32 factor=2 gain_dB=0 enabled=1 crc32=<uint> verified=1 source=verified_snapshot
rx_fir_read=<offset>,<count>
rx_fir_read=<offset> <c0> ...
fir_enable=<rx_enable>,<tx_enable>
~~~

TX 使用对应 `tx_` 前缀。read 的 count 为 1..8 且不能超过已配置 taps，返回已通过 SPI 校验的 RAM 快照；每次查询不重新读取 SPI。commit 会先读回两个物理通道的系数，并检查 taps、gain、enable 和时钟。

config=0 表示内置示例预设，config=1 表示自定义上传。旧的 `rx_fir_config=0` / `tx_fir_config=0` 恢复示例系数并关闭该方向 FIR，保持另一方向和共享采样率。`rx_fir_en=` / `tx_fir_en=` 通过相同检查事务修改启停，`fir_enable=` 则在一次事务内设置两边。

## 时钟事务

~~~text
clock_sample_rate=<Hz>
clock_begin=<token>
clock_rx=<token>,<BBPLL>,<ADC>,<R2>,<R1>,<CLKRF>,<RX_SAMPLE>
clock_tx=<token>,<BBPLL>,<DAC>,<T2>,<T1>,<CLKTF>,<TX_SAMPLE>
clock_commit=<token>
clock_abort=<token>
clock_chain?
clock_chain ref=<Hz> unit=Hz rx=<six CSV rates> tx=<six CSV rates>
~~~

`clock_sample_rate` 计算合法共享链，不能降速回退。完整链的时钟暂存槽与 FIR 槽独立，begin 替换旧 token；commit 要求同一 token 已提交两个方向，并在应用之前消耗 token。所有频率为正 uint32 Hz，RX/TX 的 BBPLL 与最终采样率必须相同，Fs 不高于 61.44 MHz。

设备检查 BBPLL 715..1430 MHz、ADC 25..640 MHz、DAC 不高于 320 MHz、可用整数分频/半带因子、FIR 因子和有效 taps 运算预算。AD9361 参考钟仍为板上 40 MHz；可通过独立 ADF4002 命令选择外部 10 MHz 驯服。PPS 频率监控见 [PPS 协议字段](pps.md)；用 PPS 装载硬件时间见 [硬件时间戳](timestamps.md)。

## 应用、校验与恢复

硬件修改要求双向 USB 和 TX 输出都已停止；否则返回 busy，不隐式停止仍运行的流。写入前 SDK 保存双向 FIR、启停、时钟与带宽，校验拟议组合、编程、双通道 SPI 读回，再检查时钟/带宽以及 RX PRBS、TX 数字回环。这些检查证明数字配置完整性，不是通阻带、群延迟或模拟 RF 性能测量。

失败时尝试检查并恢复原状态：

~~~text
ERR fir_or_clock_apply ... rollback=verified applied=restored ...
ERR fir_or_clock_apply ... rollback=failed applied=unknown ...
~~~

后一种情况把 RF/数字状态标为无效，成功 `rf_reinit!` 前禁止流。主机在任何失败硬件修改后使配置缓存失效，应用应确认恢复并重新配置。`rf_reinit!` 可重载 RAM 中保留的当前自定义 FIR/时钟；断电或重新下载 ELF 会丢失 RAM 自定义状态。

主机源码构建和安装均不编译、下载或写入 SDK、CH32、FPGA、BOOT.bin 或启动 Flash。板级固件在 [OmniCore-SDR-Firmware](https://github.com/Liren66/OmniCore-SDR-Firmware) 仓库，须按其自身流程构建和发布。
