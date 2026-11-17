# 排错与能力边界

## 找不到设备

先运行 och-driver check、och find --json，检查目标数据设备 1A86:5537 是否使用 libusbK、是否成功枚举。find 不打开设备查询链路速度，默认文本的 unknown、JSON 的 usb_speed_mbps=0 都表示未查询；继续运行 och info 检查实际 USB3 / USB2 High-Speed 链路和独占打开结果。不要把 WCH-Link 改成 libusbK。多个匹配设备必须用 --serial。

设备管理器代码 43、端口重置失败属于 USB 枚举问题，不能当作正常打开后的流故障处理。保存诊断信息后检查供电/线缆/端口。驱动被安全策略拒绝时按安装说明处理，不自动关闭签名检查或内存完整性。

## 电源查询显示 unknown

桥接固件 1.0.1 及以上时，och power / och info 每次通过 `typec?` 实时查询 USB-C 状态（`live=true`），不会出现下面的情况；仍为 unknown 时先用 `och firmware info` 确认桥接版本。以下说明只适用于桥接固件 1.0.0。

och power status 使用现有 CH32 EP83 启动日志，只有完整、无冲突且有效的 UFP 快照才能给出 supports_3a=true/false。没有 EP83、日志尚未到达、被其他程序消费或存在冲突快照都可能是 unknown，不等于不支持 3 A。info 也会读取并消费这段日志；缓存仅属于当前进程中的设备句柄，关闭句柄或退出进程后销毁。因而先 info 显示 3 A、再 power status 显示 unknown，并不表示 3 A 消失或供电下降。

启动日志不提供可靠启动编号、实时连接状态或 LED GPIO 读回。主机仅报告已有信息，不会为取得结果而修改/下载 CH32、SDK 或 FPGA，也不假装测量当前供电电流。若同一日志显示 3 A，只说明该启动快照中的检测结果，不能据此证明线缆/电源在当前负载下表现。

## 吞吐低、RX 间断或 TX 欠流

USB2 先查看 [USB2 测试结果](benchmarks.md#usb-20-high-speed)，按收发通道数与 IQ 精度选择速率。`RF sample-rate ceiling` 的 61.44 MSPS 不是 USB2 可持续速率。短测通过仍可能在几分钟后欠流，应对选定档位重复长测；默认的 30.72 MSPS 示例面向 USB3。

先运行不写盘的 packed12 benchmark，再测试文件和格式转换。双 RX 满速 wire 为 368.64 MB/s，保存 fc32 约 983.04 MB/s；磁盘短时缓存快不代表持续写入达标。

各电脑 USB 控制器、共享带宽和调度不同。可试 --buffer-mib 128，并观察 host_queue_peak_bytes、host_dropped_samples、transfer_errors 和设备溢出/欠流，不能只追求更大单块。64 MiB 约覆盖 182 ms 的双 RX 满速数据。

双向启动时短时 RX 溢出如经设备恢复并出现新完整头，基座继续接收并标明新 epoch。下游必须重置边界。持续重复溢出说明吞吐/消费能力仍不足，不能因为程序没有退出就视为无损。

有限 TX 文件回放结束时，关闭硬件前的尾部可能报告 TX 欠流和主机欠供，这是预期行为，见 [架构：TX 的显式启动与停止](architecture.md#tx-的显式启动与停止)；要与连续流中途的欠流分开判断。

## 成帧块与旧数据

`och_open` 报 `framed USB transport required`：Zynq 固件太旧，需要 Firmware v2026.11.07（FPGA `CH32H417_IP` 1.3）。旧固件请继续使用 OCH 0.5.x。

`resyncs`（终端显示为 Header errors）非零表示收到魔数或校验错误的块头，核心已按字节重新对齐。正常运行时应为 0；持续出现时检查固件版本是否配套、USB 线缆与端口。

纯 IQ 文件已去掉块头；原始 USB dump 每 16 KiB 含 16 B 头，应先按 [设备协议](protocol.md#成帧块格式) 解析，不能当作 sc16。

设备溢出事件（`RX_GAP`）可能早于或晚于对应数据到达。以数据通路的 epoch/metadata（GAP 标志和 device_dropped_samples）定位，不能在收到 EVT 时任意切断当前块。

## 重开方向返回 BUSY

当前 CH32 固件的 USB 取消/跨会话缓冲状态对单方向热重开有约束。先停双方流并销毁，再配置和创建。基座不会为了重开 TX 悄悄清空仍运行的 RX。本工程没有为解除该限制修改 CH32。

## 运行中改参、TIMEOUT 与恢复

机制与规则见 [API：运行中修改参数](api.md#运行中修改参数) 和 [架构：设备协调重配置](architecture.md#设备协调重配置)。常见现象：

| 现象 | 原因与处理 |
|---|---|
| 收发调用返回 TIMEOUT | 重配置期间的暂时中断。保留同一个句柄重试（建议间隔约 1 ms），部分 send 按 accepted 前进；持续 TX 的生产者应继续 send，不要等 configure 返回。显式 STOPPED 或其他错误不能当成超时无限重试 |
| 改参返回 BUSY | 还有未释放的 RX 租借；或已有流时想改 `rx_mask` / `tx_mask`、USB IQ 位数，需要先停止并销毁全部流 |
| RX 出现 RECONFIGURED / DISCONTINUITY，或收到零信号 | 新数据段开始，下游重置解调 / 滤波状态。重配期间的主机丢弃计入 `host_dropped_samples`，不是 DEVICE_OVERFLOW；活动 TX 恢复时会插入 4 个传输块的零 IQ（`tx_padding_samples`） |
| 改参失败后流全部停止 | 核心停止双方并使配置缓存失效。保存 `och_last_error`，结束 IO 线程、销毁流，必要时先成功执行 `rf.reinit`，再 configure 并重建流；不要在旧流上循环 start |
| 改参耗时比平时长 | 实测带宽变更最长约 873 ms、TX FIR 复写约 1057 ms，常见值不是上限 |
| 另开 `och config` / `och set` 改不了正在收发的设备 | 设备独占，没有跨进程控制服务；在应用进程内用 C/C++ 接口，见 [改参示例](../../examples/reconfigure.cpp) |

## FIR 文件或时钟被拒绝

先用 och fir status、och clock status、och get system.info 检查当前状态和 fir_upload=1 / clock_config=1 能力。FTR 的系数必须是 signed16 十进制整数，tap 数是 16 的倍数；两列固定 TX,RX。存在 RTX/RRX/BWTX/BWRX 元数据时默认拒绝，只有明确要忽略配套时钟/带宽时才加 --coefficients-only，不能用它假装完成整套频响配置。

文本合法不等于当前硬件时钟预算足够。TX factor 1 最多 64 taps；其他组合还受 ADC/DAC/Fs 比例限制。根据目标采样率调整设计；同进程 typed API 可协调暂停/恢复现有流，CLI 则仍需独占设备。SDK 失败会返回恢复状态；rollback=failed / applied=unknown 时先退出并销毁全部流，成功执行 rf.reinit，再重新配置和创建流，不能继续使用未知 RF 状态。

规范 set rx.lo-frequency 1G 使用 Hz，兼容 set rx_lo_freq 1000 使用旧 SDK 的 MHz。单位不对时先查看 och commands，不要靠尝试多个数量级猜测。

## 与第三方软件的关系

GNU Radio（gr-soapy）、SDR++、SDRangel 和 SoapySDR 的 Python / C++ 接口可以通过 SoapySDR 插件（驱动名 `omnicore`）使用 OmniCoreSDR。插件需要从源码编译，安装包不带，见 [SoapySDR 插件](soapy.md)；插件相关的问题见那里的“排错”一节。MATLAB 使用自带的工具箱（不经过 SoapySDR），问题见 [MATLAB 工具箱](matlab.md#排错)。

通过插件运行时，上面“运行中改参”一节同样适用：改 TX 本振、带宽或采样率会让传输短暂暂停，全双工下 RX 会出现空档，插件会把它报成 OVERFLOW 并重新打时间标签。插件的写入会在调用方给的超时时间内等待暂停结束，所以 gr-soapy 只有在暂停超过 100 ms 时才会报 `TIMEOUT`。

外部 PPS 开启后没有输入时显示 `no_signal`，频率无效。硬件时间戳、定时 RX / TX 见 [硬件时间戳](timestamps.md)，PPS 驯服 40 MHz 见 [PPS](pps.md)，多板同步见 [多板同步](multi-board-sync.md)。同一块板同一时间只能被一个进程打开，多个软件不能同时使用。Windows 安装器的首次驱动安装还需要在干净 Windows 主机及不同安全策略上实测。
