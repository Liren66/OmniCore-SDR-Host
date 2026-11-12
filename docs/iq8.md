# 2TX2RX 可选 signed8 USB IQ

本功能随 OCH 0.4.0 发布。默认 12bit、原单/双通道模式、板控、RGB 15%/25% 兼容及 ABI 1 均保留。已完成下列 USB 持续传输、应用转换和控制恢复实测；存在启动恢复的项目与全程无错项目分别记录。8bit 需要匹配的 FPGA IP 1.1 / revision 10 与支持 iq_format 的 SDK。

## Wire 与数值

![USB IQ 格式](images/wire-format.svg)

12bit 每通道每复样本为 3 字节 packed12。8bit 仅在 RX/TX mask 都为 3、TX source 为 USB 时选择；每时间帧为 `I1[11:4],Q1[11:4],I2[11:4],Q2[11:4]` 四字节，均为二补码 signed8。FPGA TX 恢复每个 12bit 值时补低 4 位零。通道顺序不变；每个 16 KiB 块的 16368 B 载荷正好容纳 4092 个 8bit 时间帧，块内总是整数个样点。

| 应用格式 | 每通道每复样本字节 | 8bit RX 输出 | 8bit TX 输入处理 |
|---|---:|---|---|
| packed12 | 3 | signed8 高位扩展为 12bit，低 4 位零 | 丢弃每个 12bit 值的低 4 位 |
| sc16 | 4 | signed8 × 256，左对齐 | 取 signed16 的高 8 位 |
| fc32 | 8 | signed8 ÷ 128 | 沿用旧 12bit 量化，再丢低 4 位 |
| sc8 | 2 | signed8 I/Q 原样 | 原样 |

FC32 沿用半步远离零、饱和到 -2048..2047、NaN 为零、无穷饱和的原规则。8bit 降位是删除二补码低 4 位，不改成另一个 8bit 四舍五入规则。例如 12bit 的 -1 变成 signed8 的 -1，并在 FPGA 恢复为 12bit 的 -16。

## 配置与兼容

`och_config.usb_iq_bits` 占原 `reserved[0]`，偏移 76、结构大小 104 字节，其他既有成员偏移不变。0 或 12 为默认旧格式；8 需要两个 channel mask 都为 3。`och_default_config` 置 0，旧应用的零保留槽保持原行为。`OCH_SC8=3` 新增且不改变旧枚举值；SC8 仅在 8bit wire 下创建流。

SDK `system_info?` 必须有 `iq_format=1`，8bit 还要求 PL 能力 `iq8_2t2r=1`。缺少扩展的旧 SDK 仍可配置 12bit，新主机不会向其发送新命令。每个新主机配置事务先全停并清理，在支持扩展时先恢复 12bit，再应用通道/源，最后按需选择 8bit，避免继承上一句柄遗留的 8bit。格式写后查询严格核对 requested/applied、busy/error、verified 和 scope。

存在任意流对象时改变 wire 位数返回 BUSY，需先 stop/destroy 双向流，再 configure/create。普通频率、增益、时钟等重配置保留线格式，仍使用既有暂停、清理、零预填及重启机制。RX/TX 允许按原顺序分别启动、停止；约束的是两个通道 mask，不要求两个 USB enable 在每个瞬间同时为 1。

IQ8 的 TX 启动使用 1536 raw FIFO 组门槛（与 SDK 相同），12bit 仍保留原双通道 2700/单通道 1350。第一 16KiB 包在 IQ8 下约填入 2046 主 FIFO 组，足够启动；等待旧 2700 会因 FIFO 无空间接受第二包而卡住。主机异步提交 USB 大块，轮询硬件水位，不等整个 USB 块完成才打开 TX_RUN。

`och_rx_acquire` 总是返回当前 wire，独立于应用 `format`。8bit 双通道样本数为 `bytes / 4`；12bit 双通道为 `bytes / 6`。`och_recv` 进行所选应用格式转换。所有样本索引、丢样、TX padding、预填均按每通道时间样本计；USB/ring 字节按当前 wire 计。

## CLI 与示例

```powershell
och get system.info
och get usb.iq-bits
och bench --rx --tx --channels 1,2 --iq-bits 8 --sample-rate 30.72M --seconds 10 --attenuation 89.75 --amplitude 0 --json
och bench --rx --tx --channels 1,2 --iq-bits 12 --sample-rate 30.72M --seconds 10 --attenuation 89.75 --amplitude 0 --json
```

bench 自动为 8bit 选择 sc8，为 12bit 选择 packed12，并只支持这两种原生应用格式。SC16/FC32 转换须通过 API 的 `och_send` / `och_recv` 测试；不能用 bench 或零拷贝 RX 冒充转换测试。以上命令输出零 IQ，TX 衰减为 89.75 dB，不读写 IQ 文件。

31.25 MSPS 时 8bit 双通道每方向为 125 MB/s；30.72 MSPS 时为 122.88 MB/s（双向 245.76 MB/s）；61.44 MSPS 时每方向 245.76 MB/s（双向 491.52 MB/s）。每块 16 B 头（约 0.1%）及 USB 协议开销另计。CLI JSON `usb_MBps` 为该流累计 USB 字节除以 `elapsed_seconds`，含启动/停止开销；不是独立稳态窗口的速率。

`examples/duplex_iq8.cpp` 展示双通道 SC8 接收和双通道零 IQ 发送，使用独立 TX producer。改变应用 args.format 可用 SC16/FC32/packed12，缓冲按应用格式尺寸分配。

通过 SoapySDR 插件使用时，在设备参数里写 `iq_bits=8`（如 `driver=omnicore,iq_bits=8`）。这时两个通道都要开，原生格式为 CS8，也可以用 CS16 / CF32。全双工还要写 `rx_mask=3,tx_mask=3`。2T2R 61.44 MSPS 在 GNU Radio 中的验收结果见 [SoapySDR 插件](soapy.md)。

SDK 控制命令为 `iq_format?`、`iq_format=8` 和 `iq_format=12`；规范 CLI 名为 `usb.iq-bits`。CH32 数据/控制固件保持透明传输。新 SDK 查询包括 `iq_format=8 requested=8 applied=8 supported=12,8 cfg_busy=0 cfg_error=0 verified=1 scope=2tx2rx`；后续诊断字段允许附加。

## 实测性能

| 主机 | 配置 | 时长 | RX / TX MB/s | 溢出 / 欠流 / 主机丢样 |
|---|---|---:|---:|---|
| Windows 11，USB3 5 Gbit/s | 8bit 2RX + 2TX，61.44 MSPS | 300 秒 | 245.7 / 245.7 | 0 / 0 / 0 |
| Ubuntu 24.04，USB3 5 Gbit/s | 8bit 2RX + 2TX，61.44 MSPS | 300 秒 | 245.8 / 245.7 | 0 / 0 / 0 |
| Windows 11，USB3 5 Gbit/s | 8bit 2RX + 2TX，50 MSPS | 300 秒 | 200.0 / 200.0 | 0 / 0 / 0 |

理论 wire 速率为 245.76 MB/s（61.44 MSPS × 2 通道 × 2 字节）。结果与主机、USB 控制器和端口有关，不代表任意主机长期零丢样；50 MSPS 档位留有吞吐余量。
