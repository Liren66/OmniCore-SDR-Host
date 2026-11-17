# AD9361 FIR 与时钟

这些命令控制 AD9361 芯片内的 RX/TX FIR。它们不对主机收到的 IQ 再做软件滤波，也不提供滤波器频响设计器。文件解析成功、CRC 一致和硬件读回成功只证明配置按要求装载；不能由此声称已经测量通带、阻带、群延迟或模拟射频性能。

## 常用流程

~~~powershell
och clock status
och fir status --json
och clock sample-rate 30.72M
och fir rx load my_filter.ftr
och fir tx load my_filter.ftr
och fir both on
och fir status --json
och bench --rx --tx --sample-rate 30.72M --channels 1,2 --seconds 10
och fir both off
~~~

先为实际采样率设计/选择系数，再按文件声明的插值/抽取因子和当前时钟确认可用性。上述命令顺序是假定这组系数适合 30.72 MSPS 的示例，不代表任意文件都可以使用此采样率。某些 RX/TX 因子组合只能同时启用，用 both on 可在一个事务里设置两边；只启用 rx 或 tx 会保持另一边当前的启停状态。

每次 load 仅装载指定方向，成功后该方向关闭，不隐式发射。RX1/RX2 共用一组 RX 系数，TX1/TX2 共用一组 TX 系数；本版不支持同一方向两路使用不同 FIR。采样率和 AD9361 时钟链仍由 RX/TX 共用，改变一个方向不能被理解为独立改时钟。

C/C++ 的 FIR / 时钟写接口可以在流运行时调用，由设备协调暂停、应用、恢复，规则与失败处理见 [API：运行中修改参数](api.md#运行中修改参数)。SDK 对每次硬件事务做状态检查、两通道系数 SPI 校验、时钟回读和数字通路检查；若错误显示 `rollback=failed`、`applied=unknown`，必须先完成 `rf.reinit` 并确认成功。独立 CLI 命令仍要独占打开设备，不能改另一个进程正在收发的设备。

加载的自定义 FIR 保存在设备 SDK 的 RAM 状态中，后续正常 configure/流命令会保留它。断电或重新下载 ELF 不保证保留；安装程序不写启动 Flash。旧 SDK 需具备 system_info? 中的 fir_upload=1、clock_config=1，见 [固件依赖](firmware-compatibility.md)。

## 支持的 FTR 格式

支持 ADI FTR 文本的系数部分和明确识别的配置字段，采用以下大小写和语法。十进制整数，不允许浮点、指数或 k/M/G 系数后缀；UTF-8 BOM、CRLF、空行、行尾 # 注释可以接受。

~~~text
# 两列时固定为 TX, RX；本段仅展示语法，省略号不是合法系数。
TX 3 GAIN -6 INT 2
RX 3 GAIN 0 DEC 2
12, -34
56, 78
...
~~~

只有一个方向时只需对应 RX 或 TX 头。单列表示该列供文件声明的方向使用；两列必须同时声明 RX/TX，第一列 TX、第二列 RX。整个文件不能混用单列和双列。所有头和配置字段必须在系数之前。

工程附带 [identity16.ftr](../../examples/fir/identity16.ftr) 作为可加载的格式/事务测试文件：两列分别 TX/RX、factor=1、gain=0、首 tap 为 32767、其余 15 tap 为 0。它不是推荐低通滤波器，也不承诺精确单位增益或实测 RF 频响，不能代替业务所需的滤波器设计。

| 字段 | 约束 |
|---|---|
| 通道 mask | 当前选中方向必须为 3，即该方向两个物理通道 |
| 每个系数 | signed 16 位整数，-32768..32767 |
| tap 数 | 16..128，必须为 16 的倍数 |
| RX DEC / TX INT | 1、2 或 4 |
| RX GAIN | -12、-6、0、6 dB |
| TX GAIN | -6、0 dB |
| TX INT 1 | 最多 64 taps |
| 文件 / 行 | 最多 256 KiB / 4096 字节 |

这些只是静态边界。实际可用 tap 数还受当前 ADC/DAC 时钟、采样率和另一方向配置限制。合法的 128-tap 文本可能在某个高速时钟组合下被硬件校验拒绝；程序不会擅自把采样率退到更低的值来让它通过。

解析器拒绝未知字段、重复头、重复配置字段、非完整整数、越界、超长或二进制内容。在打开设备之前完整验证文件，错误输入不会开始上传。

## 完整 FTR 时钟/带宽配置

ADI 生成的文件可能还带以下字段：

~~~text
RTX <BBPLL> <DAC> <T2> <T1> <CLKTF> <TX_SAMPLE>
RRX <BBPLL> <ADC> <R2> <R1> <CLKRF> <RX_SAMPLE>
BWTX <Hz>
BWRX <Hz>
~~~

本版 FIR loader **不会自动应用这些时钟与带宽值**。默认拒绝含这些字段的文件，防止系数已加载、配套时钟却未应用的含糊状态。若确实只需要其中系数、增益和因子，可明确运行：

~~~powershell
och fir rx load full_profile.ftr --coefficients-only --json
~~~

程序仍验证上述字段是否为正确数量的正 uint32 十进制 Hz，并在 stderr 给出提示、在 JSON ignored_profile_fields 列出被忽略字段。忽略不是成功导入完整滤波器配置，不能据此保证设计频响。

需要完整配置时，应根据设计要求通过 clock sample-rate 或 clock set 设置合法链，并通过 set rx.rf-bandwidth / set tx.rf-bandwidth 设置模拟带宽，随后核对实际状态。更改采样率会改变同一组系数对应的 Hz 频响；必须重新评估或重新设计系数。

## 时钟命令

clock status 回读 reference_hz 和两组六级时钟。clock sample-rate 30.72M 让 SDK 计算合法共享链，明确拒绝超出时钟或 FIR 范围的组合。若只是小数分频无法精确命中（如 11.2 MSPS），会改用最近的可精确实现的整 Hz 速率（11200011 Hz，详见 [API 参考](api.md)），并以 `WARN`（JSON 模式为 stderr 的 `sample_rate_adjusted`）给出请求值与实际值。set rx.sample-rate、set tx.sample-rate、set clock.sample-rate、config 和流命令的 `--sample-rate` 也使用同一入口。

需要精确指定链时：

~~~powershell
och clock set --rx-chain "983.04M,245.76M,122.88M,61.44M,30.72M,30.72M" --tx-chain "983.04M,245.76M,122.88M,61.44M,30.72M,30.72M"
~~~

这是 FIR 旁路时的链格式示例，实际需与启用的 FIR 因子匹配。每组必须恰好六个正整数 Hz，可带 k/M/G 后缀，逗号内不放空格。RX 顺序 BBPLL、ADC、R2、R1、RF、采样率；TX 顺序 BBPLL、DAC、T2、T1、TF、采样率。两边 BBPLL 和最终采样率必须一致；合法分频、时钟范围及 FIR 运算预算由 SDK 再检查。成功输出为实际回读结果。

AD9361 参考钟为固定 40 MHz，不能通过 clock set 修改。0.3.2 可用 ADF4002 将这个 40 MHz 振荡器驯服到外部 10 MHz，详见 [参考与外部 LO](reference-lo.md)。PPS 驯服见 [PPS](pps.md)，定时收发见 [硬件时间戳](timestamps.md)，多板采样时钟同步见 [多板同步](multi-board-sync.md)；外部参考选源本身不构成相干同步或锁定证明。

## C/C++ 对应接口

och_get_fir / och_set_fir / och_set_fir_enabled 对应 FIR 状态、配置及两个方向启停；och_get_clock / och_set_clock / och_set_sample_rate 对应时钟。C++ 的 device 提供 fir、set_fir、set_fir_enabled、clock、set_clock、set_sample_rate 同名包装。

fir 状态中的 verified 表示 SDK 保存的配置已通过 SPI 校验；查询系数来自最近一次已验证快照，并非每次查询都重新读 SPI。CRC32 检查上传/快照的系数一致性，不代表频响合格。API 不接收文件路径：上层可以自行设计系数并填入 och_fir_config；FTR 文件解析目前是 CLI 的功能。

格式背景参考 [ADI AD9361 驱动文档](https://analogdevicesinc.github.io/linux/drivers/iio-transceiver/ad9361/) 与 [ADI Filter Wizard](https://github.com/analogdevicesinc/ad936x-filter-wizard)。本项目没有复制其滤波器设计实现。
