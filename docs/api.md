# C/C++ 开发接口

## 链接与 ABI

公共 C 头文件为 omnicore/och.h；Windows 链接 omnicore.dll 的对应工具链导入库。omnicore/och.hpp 是调用方编译的 C++17 RAII 包装。发行 SDK 同时提供由 DLL 导出表生成的原生 MSVC omnicore.lib，以及 MinGW 的 libomnicore.dll.a；它们不能靠改扩展名互换。

适配器项目可以通过安装包的 CMake 配置选取对应导入库：

~~~cmake
cmake_minimum_required(VERSION 3.20)
project(my_receiver LANGUAGES CXX)
find_package(OmniCoreHost 0.3 CONFIG REQUIRED)
add_executable(my_receiver receive.cpp)
target_compile_features(my_receiver PRIVATE cxx_std_17)
target_link_libraries(my_receiver PRIVATE OmniCore::omnicore)
~~~

配置时将 CMAKE_PREFIX_PATH 指向 OmniCore Host 安装目录。运行时让安装目录的 bin 在 PATH 中，或把匹配版本的 DLL 放到应用目录。实际 MinGW/MSVC 消费者构建验证范围以发行验证报告为准。

带 struct_size 的输出结构先清零并设为 sizeof(结构体)；配置/流参数优先调用 och_default_config、och_default_stream_args 初始化。ABI 版本为 OCH_ABI_VERSION，软件版本来自 och_version()。插件不要直接依赖内部结构。

错误返回 och_result。och_last_error() 提供调用线程的失败详情，应立即复制；下一次失败可能覆盖。C++ 控制方法通过 och::check 抛异常，而 recv/send 保留 TIMEOUT/STOPPED 返回值供循环处理。

## 生命周期

1. list 枚举，open 用序列号打开。不指定序列号只选择唯一设备。
2. configure 设置共享采样率、mask、LO、带宽、增益，不隐式发射。
3. create_stream 为方向创建一次性流并分配缓冲。
4. RX：start 后 recv 或 acquire/release。TX：send 预填后 start，再持续 send。
5. TX 最后一个 send 后可 flush；flush 期间不得并发 send。
6. stop，等待应用 IO 线程退出，释放租借，destroy_stream。
7. 所有流销毁后 close；仍存在流时 close 返回 BUSY。

显式 stop 后不可再次 start，要销毁并重建。当前 CH32 缓冲恢复约束要求一方向停止后重开前先停另一方向；若另一方向仍活动则返回 BUSY。0.3 起，运行中的参数修改由设备统一协调，应用无需为每次改参 stop/destroy/create；内部暂停不会使原本运行的流成为一次性终止状态。详见下一节。

device 必须比其 stream 活得更久。示例把 device 声明在 stream 之前，依靠逆序析构。析构清理不用于报告关键错误，正常停止建议显式 stop/close 并处理返回值。

destroy_stream 返回 BUSY 表示仍有租借，句柄保留；OK 或 IO 都已销毁句柄，即使停止硬件时出现 IO 错误也不能再次 destroy。C++ close 已处理该所有权规则。设备 close 的 BUSY 则保留设备对象，待所属流销毁后重试。

get_config 返回该句柄最近一次成功应用的配置请求，仅在该句柄已 configure 时可用。它不是所有射频寄存器的实时回读；例如 LO/时钟的硬件量化、AGC 实际增益变化不会自动反映在此缓存。需要核对实际状态时可查询对应 SDK 命令，不能把请求值当成测量值。

open 会取得设备所有权、确认协议版本、停止旧 USB 数据会话并使用现有 CH32 诊断接口清理跨会话数据。它不是完全无副作用的旁路观察接口。独立 CLI 的 info/get/config 也需要 open；如果已有 OCH 进程持有设备，第二个进程会被拒绝。list/find 仅枚举，不接管数据流。

## 运行中修改参数（0.3）

在持有设备的**同一进程**中，管理线程可直接调用 och_configure / device.configure，或 typed FIR、clock、sample-rate、RX correction、reference、LO source 设置接口。核心协调暂停 RX/TX、等待当前 IO 操作退出临界区、应用参数、清理跨会话数据，再恢复修改前正在运行的方向。原 och_stream 句柄、格式、USB 请求缓冲和主机大缓冲保持稳定，调用方继续使用同一个流对象。

- 已有任意流对象时，rx_mask、tx_mask 都不能改变；改变通道布局必须先销毁所有流。流格式和缓冲尺寸仍在 create_stream 时确定。
- configure 的有效配置与已成功应用的请求完全相同时直接返回，不中断收发。有变化时只写变更的射频配置，避免无关的时钟/带宽写入和校准；这不承诺每种参数都能无缝修改。
- 仅恢复原本 running 的方向。尚未 start 的流仍未启动；已显式 stop 的流仍保持停止。只接收的应用不会因此自动发射。
- 正在等待的 recv、acquire 或 send 可暂时返回 OCH_TIMEOUT；消费者/生产者保留同一个句柄并重试。重配置期间 TIMEOUT 可能立即返回，重试前建议短暂休眠（例如 1 ms），避免忙循环占用 CPU 和反复争抢 I/O 锁。send 已接受部分样本时仍返回 OCH_OK，必须按 accepted 前进，不能重复提交已接受前缀。
- 有 RX 租借尚未 release 时返回 OCH_BUSY，硬件不变。使用租借的应用应先与消费者协调释放，再重试改参；不能一边持有指针一边等待重配置完成。
- 控制和生命周期调用由一个管理线程串行执行。destroy/close 不得与该设备/流上的任何操作并发；flush 也不应与改参或 send 并发。

RX 重配时丢弃旧队列，已知的本地主机丢样计入 host_dropped_samples。恢复后等待新的完整启动头，epoch 保持单调递增，首块带 OCH_META_START | OCH_META_DISCONTINUITY | OCH_META_TIME_UNKNOWN | OCH_META_RECONFIGURED（新增值 32）。重配本身不伪装成 DEVICE_OVERFLOW；设备侧停顿和丢样仍不可精确计数。下游应在该边界清除滤波、同步和解调历史，并使用新配置解释后续数据。

TX 重配时丢弃旧排队数据并取消旧 USB 请求，避免把旧配置下的样本重放到新配置。原本正在 TX 的方向恢复前自动预填 4 个 transfer_bytes 大小的零 IQ 块，样本数计入 tx_padding_samples；新配置与零块提交后，先放行原生产者继续 send，在硬件启动过程中补充队列，再先恢复 RX、最后启动 TX。生产者应在 TIMEOUT 后继续重试，不必等待管理线程的配置调用返回；若恢复失败，核心重新阻止 TX 数据操作并停止双方。尚未启动或已经停止的 TX 不会因此启动。此前 send 接受的样本不保证已从 DAC 发出，也可能在重配中丢弃。

### 快速调谐（0.4.6）

如果相对上次成功的配置，只改了 rx_frequency、tx_frequency、rx_gain、tx_attenuation 中的若干项，configure 会**直接下发命令，不暂停流**：不清队列，不重置 USB 传输，不预填 TX 零块，epoch 不变。完成后，下一个 RX 块带 OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY。这个边界是近似的：命令返回时还在 USB 队列里的样本（约 usb_queue_depth × transfer_bytes）可能已是新频率，也可能是 PLL 稳定过程中的数据。需要精确边界的应用应在标记附近丢弃一小段样本。

- 有 TX 流时改 tx_frequency，仍走上面的暂停事务。原因是 TX LO 跳变超过 100 MHz 会触发 TX quad 校准，不宜在发射过程中进行。
- 手动 RX 增益只在 LO 跨越 SDK 增益表频段（1.3 GHz / 4 GHz，边界 ±1 MHz 内按跨越处理）或增益本身变化时重写。
- 快速调谐中途失败时，按重配置失败处理：停止双方，配置缓存失效。
- 成功后发出 OCH_EVENT_RECONFIGURED，消息为 `retune complete streams_continued=1 ...`。
- 能力表（system_info?）在 open 时读取并缓存；外部 LO 选择也会缓存，直到 set_lo_source 或 raw 修改命令使它失效。

实测（USB3，30.72 MSPS RX 流运行中）：同频段 1 MHz 步进约 11 ms，2400↔900 MHz 跨增益表约 47 ms，仅改 RX 增益约 7 ms，期间 RX 速率保持 30.71 MS/s。0.4.5 的同类调用约 275 ms。时间主要花在 115200 波特控制串口的往返上。

OCH_EVENT_RECONFIGURED（新增值 6）报告成功重配的恢复方向、间断/丢弃及主机测得的耗时，失败通过 OCH_EVENT_TRANSPORT 报告。事件是控制操作的观察记录，IQ 边界以 RX 元数据为准。单通道 RX、零 IQ TX 和双向模式已完成实机改参恢复测试。其中耗时属于主机 API 和观察时间，不是硬件采样间断，也没有固定的毫秒级延迟保证。

硬件变更或恢复失败时，双方流停止，设备配置缓存失效，不自动回滚并重新发射。应用应保留错误详情，停止并等待 IO 线程退出，销毁旧流，在确认设备状态后显式 configure 并重新创建/启动流；若 SDK 报告 rollback=failed / applied=unknown，需按 FIR 说明先完成 rf.reinit。参数校验失败或租借导致的 BUSY 不属于已执行硬件事务失败。

这项能力不提供跨进程控制服务：另开一个 och.exe 不能修改正在接收的软件所持设备。原始 och_command 修改仍禁止在存在流对象时执行，不能用 raw 命令绕过协调接口。本版不附带 OnAir 数据源或 SoapySDR 驱动。参见 [持续接收并改参示例](../examples/reconfigure.cpp)。ABI 仍为 1，已有公共结构布局不变；新语义需匹配 0.3 或更新 DLL，RX correction 新增函数须匹配含这些导出的 DLL。

## RX 直流与 IQ 校正

0.3.1 新增独立 och_rx_correction_config，不扩展 och_config 或 och_device_info。输出先清零并填写 struct_size；dc_enabled 和 iq_enabled 均为 0/1，表示 AD9361 原生跟踪校正的开启状态。

| C 函数 | C++ device 包装 | 用途 |
|---|---|---|
| och_get_rx_correction | rx_correction() | 回读 DC 和 IQ 两个当前状态 |
| och_set_rx_dc_correction | set_rx_dc_correction(bool) | 同时控制 RF 与基带 DC offset tracking |
| och_set_rx_iq_correction | set_rx_iq_correction(bool) | 控制 RX quadrature tracking |

两项设置都作用于两个物理接收通道，不随 rx_mask 选择而变成逐通道开关。它们是芯片的接收跟踪功能，不进行主机 IQ 数据处理，也不触发 TX IQ 校准。0 表示停止持续跟踪更新，不清除已经得到的校正系数；IQ=0 仍保留硬件相位/增益校正路径，并非完全旁路 IQ 校正。1 表示启用跟踪，不表示已测量直流残留或镜像抑制度。

~~~cpp
auto correction = dev.rx_correction();
dev.set_rx_dc_correction(true);
dev.set_rx_iq_correction(true);
~~~

每次先检查 system_info? 的 rx_correction=1，缺失或不是 1 返回 OCH_UNSUPPORTED，已有流继续运行。查询不会暂停流。setter 拒绝非 0/1 值；与设备回读相同的设置直接成功，不清队列、不产生新 epoch。确实需要修改时，使用本节前述协调流程暂停双方；完成后回读两项状态，确认目标值生效且另一项未改变，再恢复原本运行的方向。写入失败或回读不一致会停止双方并使配置缓存失效，应用按重配置失败流程恢复。

正常 configure 不写这两个开关，SDK 的 RF 重建、FIR/时钟操作会恢复最近成功保存的跟踪目标；rf_reinit 也恢复该目标。冷启动默认均为 1，断电/重新下载程序不保留 RAM 中的关闭请求。get_config 不包含它们，应使用专门的 getter 查询。SDK 检测到 RFDC/BBDC 混合状态或 SPI 失败会报错，不把混合状态伪装为 0；如果错误报告 rollback=failed / applied=unknown，需要先 rf_reinit 再重新配置和建流。缺少此能力的旧 SDK 仍可使用原先支持的功能，不应以固件名字替代能力字段判断。新 API 必须配套 0.3.1 或更新的 omnicore.dll 与头文件。

## USB-C 启动电源快照

0.2.1 新增 och_get_power_status(device, &status, timeout_ms)，C++ 为 device.power_status(timeout_ms=500)。och_power_status 先清零并填写 struct_size；函数读取现有 CH32 EP83 调试日志中的 HD3SS3220 启动寄存器快照，不发起新的固件查询、不改变板上软件。

timeout_ms=0 只返回当前 device 句柄内的缓存；1..2000 允许在没有流对象时有界读取 EP83。有流对象时只读缓存，不与高速流争用日志端点。设备关闭后缓存销毁，不写磁盘。成功读取会消耗该 USB 调试端点的数据；另一个程序此前读取过日志时可能没有可用快照。

缺日志、没有 EP83 或等候超时均可正常返回 OCH_OK，同时 snapshot_valid=0、supports_3a=-1。返回 OK 只表示查询完成，不保证电源状态已知。snapshot_valid=1 且 conflicting_snapshots=0 后才可解释 raw_08/raw_09/raw_0a、attached_state、current_detected。仅有效 UFP 连接的 supports_3a 可为 0/1，其余为 -1；source_current_ma=-1 表示未知，包括 USB 默认电流公告，不应擅自填成 500/900 mA。

日志没有可靠启动编号或采样时间戳，彼此冲突的快照保持未知。它不是实时电流/电压测量、USB-PD 合约、LED GPIO 读回或电源在负载下的能力测试。CLI 将未知数值转换成 JSON null，并固定标注 source=startup_log、live=false；应用同样应保留这些限制，不能只把非零 supports_3a 当作 true，因为 -1 本身也非零。

调用此新 API 必须匹配 0.2.1 或更新的 DLL。既有 och_device_info 布局不变；och info --json 在原字段之外新增 power 对象，och find 仍只枚举设备。

## FIR 与共享时钟

0.2 新增 och_fir_config、och_clock_config 及对应函数，已有 ABI 1 结构布局保持不变。调用新函数必须使用包含这些导出的 0.2 或更高版本 DLL；头文件升级不能让旧 DLL 自动拥有新能力。

| C 函数 | C++ device 包装 | 用途 |
|---|---|---|
| och_get_fir | fir(direction) | SDK 最近一次 SPI 校验的 FIR 配置快照 |
| och_set_fir | set_fir(config) | 分块事务上传、校验并设置一个方向 |
| och_set_fir_enabled | set_fir_enabled(rx,tx) | 在同一事务内设置两边启停 |
| och_get_clock | clock() | 实际参考钟与两组六级时钟回读 |
| och_set_clock | set_clock(config) | 检查并设置完整 RX/TX 链 |
| och_set_sample_rate | set_sample_rate(hz) | SDK 安全计算并应用共享采样率；无精确时钟方案时改用最近可用速率 |

FIR 配置先清零、填 struct_size、direction、channel_mask=3、factor、tap_count、gain_db、coefficients，再选择 enabled。CLI 的 load 默认 enabled=0；直接 C/C++ API 可明确填写启用状态。verified 和 crc32 为读取结果，设置时由核心计算 CRC 并验证。tap 数为 16 的倍数且最多 128，signed16 系数，RX gain 为 -12/-6/0/6 dB、TX gain 为 -6/0 dB、factor 为 1/2/4；TX factor 1 最多 64 taps，硬件时钟预算还可能降低上限。同方向两个通道使用同一组系数。

clock 的 rx_hz 是 BBPLL/ADC/R2/R1/RF/Fs，tx_hz 是 BBPLL/DAC/T2/T1/TF/Fs，单位均 Hz。reference_hz 读取固定 40 MHz 板级参考钟，写入 0 表示保持。0.3.2 的 `och_set_reference_source` 可通过 ADF4002 启用外部 10 MHz 驯服，但 AD9361 输入仍为 40 MHz，不能通过 `och_set_clock` 改这个值。RX/TX 的 BBPLL 和 Fs 必须一致。合法分频和 FIR 约束由 SDK 验证，不接受隐式降低采样率来满足限制。

**最近可用采样率**：BBPLL 为小数分频（40 MHz 参考，模数 2088960），驱动按最近的小数字编程、回读时向下取整，SDK 要求回读与请求逐 Hz 一致，因此部分速率会被拒绝，例如 11.2 MSPS 需要 716.8 MHz 的 BBPLL。`och_set_sample_rate` 与 `och_configure` 在主机端复现 AD9361 驱动 `ad9361_calculate_rf_clock_chain` 的时钟链选择（按当前 FIR 抽取 / 插值因子）及 BBPLL 舍入，从请求值开始按 +1、−1、+2、−2 Hz…… 找到第一个能精确实现的整 Hz 速率，只向设备提交一次。11.2 MSPS 会变为 11200011 Hz，偏差通常在 20 Hz 以内（全范围建模最大约 33 Hz）。若设备仍拒绝（固件与模型不一致），再在设备上逐 Hz 尝试，最多 ±64 Hz。实际速率写入 `och_get_config` 的 `sample_rate`；同一句柄再次请求原速率时直接沿用。超出时钟或 FIR 范围的速率（如未启用 FIR 抽取时的 1 MSPS）仍按原错误失败。`och_set_clock` 指定的完整时钟链不做替换。

0.3 起，typed FIR/clock 写操作使用上述协调重配置流程，可在已有流时调用；所有管理操作仍应串行执行。硬件写入失败后双方停止，销毁旧流，在必要的 rf.reinit 成功后重新 configure 并创建流；不要把失败前缓存当成已知实际状态。成功的 FIR 修改保留当前 Fs；正常 configure 保留已装载的 FIR。换 Fs 后要重新确认频响。原始命令 och_command 的修改仍要求无流对象，且会使配置缓存失效。

FIR/clock/sample-rate 的能力检查与固定参考钟拒绝在暂停流之前完成，旧 SDK 不支持扩展或 reference_hz 请求与固定板级参考钟不符时，不停止已有流、不清空 IQ 队列。实际写入开始后的失败仍按重配置失败处理。

FTR 解析属于 CLI，公共 C API 接收已经验证的定长系数数组，不依赖文件格式。verified/CRC32 证明配置一致性，不是滤波器频响或 RF 性能测量。详细文件格式和生命周期见 [FIR 与时钟说明](fir.md)。

## ADF4002 参考与外部 LO

参考与 LO API、PPS 频率 LOCK（0.3.4）、外部频率声明和内部频率恢复语义见 [ADF4002 与外部 LO](reference-lo.md)。`och_config` 不包含源选择字段；`configure` 保留独立选择，LO source setter 成功后同步配置里的实际名义频率。默认源由 SDK 启动配置决定，RX/TX 均为内部。

## 样本与格式

recv 的容量/返回数量、send 的输入/接受数量均为**每通道复样本数**，不是标量数或所有通道总数。双通道按时间优先：I1(t0),Q1(t0),I2(t0),Q2(t0),I1(t1),Q1(t1)...。只选通道 2 时仅有该物理通道 I/Q。

| 格式 | 每通道每复样本 | 定义 |
|---|---:|---|
| packed12 | 3 B | I=(b0<<4)\|(b1>>4)，Q=((b1&15)<<8)\|b2；分别符号扩展 12 位 |
| sc16 | 4 B | 小端 int16 I/Q，wire 有符号 12 位值左对齐 4 位 |
| fc32 | 8 B | 小端 float I/Q，wire 值除以 2048 |
| sc8 | 2 B | signed int8 I/Q，仅在 usb_iq_bits=8 下可用 |

双通道 recv 容量 65536 时，sc16 缓冲至少 65536×2×2×sizeof(int16_t) 字节。16 位格式不是未经缩放的 -2048..2047。

TX 的 sc16 会丢弃低 4 位；fc32 按 12 位量化，半步远离零取整，超范围值饱和到 -2048..2047，NaN 映射为零。适配器不应依赖超范围输入保留原值。内部可选择 CPU 支持的向量化实现，公开格式和数值语义保持一致。

`och_config.usb_iq_bits` 使用原首个 reserved 槽，结构仍为 104 字节，ABI 1 不变。0/12 使用上述默认 12bit wire；8 仅允许 `rx_mask=tx_mask=3`，且需要 SDK `iq_format=1 iq8_2t2r=1`。任何流对象存在时不能改变 wire 格式，需要先销毁流。8bit RX 将 signed8 恢复为 sc16×256、fc32÷128 或低 4 位为零的 packed12；TX 使用原 12bit 量化后丢掉低 4 位。应用格式的尺寸不随 wire 改变；SC8 直通。详见 [8bit 扩展](iq8.md)。

## RX 元数据和租借

一次 recv/acquire 不混合多个 epoch 或已知间断：

- epoch：每确认完整 16 KiB 头增加，不是硬件帧序号。
- sample_index：当前 epoch 内每通道位置，不是硬件时间戳。
- flags：START、DISCONTINUITY、主机溢出、设备溢出、时间未知、RECONFIGURED。
- host_dropped_samples：已知的主机本地丢样。
- device_dropped_samples=-1：设备没有提供精确损失。
- host_monotonic_ns：主机观察时刻，不是 ADC 采样时刻。

数据头与 EVT 从不同 USB 通道到达；EVT 的到达时刻不能替代数据边界。消费者应依据 IQ 元数据重置同步、滤波历史或帧解析。

och_rx_acquire 返回当前 wire 格式的租借，容量单位为字节，与 args.format 无关。12bit 每复样本 3 字节，8bit 每复样本 2 字节；双通道分别为每时间帧 6/4 字节。每流最多一个租借，释放前不得再次 acquire、recv、destroy；运行中重配在存在租借时返回 BUSY。release 后指针立即失效。保持处理时间有界，避免长时间占用核心队列。

## 部分完成与时间

send 可能只接受部分样本；一旦接受了样本就返回 OCH_OK，调用方始终按 accepted 消费对应前缀，再重试剩余部分，不能重发整块。TIMEOUT 表示期限内未接受样本或因协调重配置暂时中断，不等于永久断开。

start 前 send 只预填不发射。停止后不会因为后来提交数据自动开启。flush 对齐补零并等待主机队列与 OUT 完成；补零数量见 tx_padding_samples。它不提供 DAC 播放完成的硬件时刻，本版不支持 timed TX、PPS 对齐的采样时间、硬件 timestamps 或跨设备相干同步。独立的 PPS 接口为 `och_set_pps_enabled`、`och_get_pps_status` 和 `och_get_vcxo_status`（内部参考时 PPS 经 CH32 PA4 驯服 40 MHz），详见 [PPS API](pps.md)。

有限文件 EOF 的 flush 完成与 stop 关闭硬件之间没有设备端 end-of-burst 握手。此时 FIFO 排空可能触发 tx_underflow，应用队列已结束也可能触发 TX_STARVED；事件及计数原样保留。调用方应把“已结束供应后的尾部欠供”和“预期连续供应期间的欠供”分开记录，不能仅据错误计数判定持续流中段丢样，也不能把全部 USB 完成解释为精确定长 DAC 播放成功。若业务需要精确 burst 结束点，必须增加设备端协议/硬件能力，当前 API 不提供该保证。

## 事件与线程

每设备有有界事件队列，och_poll_event 获取设备 EVT、RX 同步、主机溢出、传输故障、TX 欠供和重配置结果等。应用应定期取出，event_drops 增加表示队列未及时处理或事件过密。

RX 与 TX 可并行；同一 RX 仅一个 consumer，同一 TX 仅一个 producer。stop 和协调重配置可与正在进行的收发并行，用于结束或暂时中断等待；destroy/close 不能与该句柄上的任何操作并发。确认 IO 线程退出后再销毁对象。配置与生命周期操作由应用串行管理。

och_command 是 SDK 原始控制入口，修改型命令只允许没有流对象时使用；之后应 configure 重新建立一致配置。插件应优先使用标准配置 API，不单独打开相同 USB 设备与基座竞争。

## 统计口径

- usb_bytes 是该流已完成 USB 请求的字节数；RX 包括传输头，TX 不包括仅接受进入主机队列但尚未完成 USB 的部分。
- samples 在 RX 表示已交给应用的每通道样本数，在 TX 表示已接受进入主机队列的每通道样本数。两者都不是 RF 测量，尤其 TX 不能用它断言全部样本已经发射。
- host_dropped_samples、headers、resyncs、transfer_errors、tx_starvation_events 属于该流；队列字节和峰值按当前 wire 字节计，12bit 为每复样本 3B，8bit 为 2B。
- device_overflows、device_underflows、event_drops 属于该设备对象的观察期；RX/TX stats 会看到相同设备计数，不要把两份相加。设备错误字段按收到的相关 EVT 通知计数，可能受固件通知合并影响，不能当作精确物理 FIFO 错误次数或丢样量。
- 最大采样率 61.44 MSPS 是单通道的射频能力，不表示 USB 可同时承载四通道都满速。四通道 61.44 MSPS 总 wire 需求 737.28 MB/s；应按共享 USB 带宽选定双向速率，常用四通道测试使用 30.72 MSPS。

## RX RSSI（0.4.7）

`och_get_rx_rssi(device, &rssi)` 返回 `och_rx_rssi`，包含 `enabled`、`usb_rx_mask`、`valid[2]` 和 `rssi_db[2]`；`och_set_rx_rssi_enabled(device, 0|1)` 用于开关。C++ 接口为 `device.rx_rssi()` 和 `device.set_rx_rssi_enabled(bool)`。RSSI 上报在 SDK 冷启动后默认关闭，此时 `enabled=0`，两路都无效。数值是 AD9361 symbol RSSI，单位为相对 dB，越大越强，不是校准过的输入功率。ENSM 不在 RX/FDD 状态时 `valid=0`、`rssi_db=0`；`usb_rx_mask` 只表示 USB 上传掩码，2R2T 下两路接收机都在测量。这是纯查询，可以在 RX/TX 流运行时调用，不会暂停流，实测双通道 30.72 MSPS 接收时无间断。新 SDK（`rssi=1`）使用 `rx_rssi?`；旧 SDK 从 `system_status?` 中解析，同样可用，但较慢。

## 0.3.5 板级 RGB 与风扇温控

新增 RGB 开关/状态、风扇温控开关/状态和 Zynq 核心温度查询。使用新 FPGA BOARD_CTRL_AXI 及新版 SDK，能力标志为 `board_rgb=1 fan_control=1 core_temp=1`。当前 SDK 默认 RGB 开启、亮度 15%，风扇温控关闭；OCH 0.3.6 同时兼容旧版 25% 固件，亮度以设备回报为准；不改变已有 C ABI 结构或停止 IQ 流。详见 [板级控制及接口](board-control.md)。

## 桥接固件（0.5.0）

`och_firmware_query`、`och_firmware_read_image`、`och_firmware_update`、`och_firmware_enter_bootloader`、`och_firmware_start_app` 只使用 EP0 厂商请求，不打开 SDK 控制通道，可在 Bootloader 模式下使用；设备被其他句柄占用时返回 `OCH_BUSY`。`och_get_firmware_info` 返回已打开设备的桥接固件信息。设备处于 Bootloader 时 `och_open` 返回 `OCH_UNSUPPORTED`。用法和恢复流程见 [桥接固件升级](firmware-update.md)。
