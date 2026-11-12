# MATLAB 工具箱

`bindings/matlab` 让 MATLAB 直接使用 OmniCoreSDR，不经过 SoapySDR，也不依赖任何工具箱。它由两部分组成：

- **MEX 网关** `omnicore_mex`：只调用公共 C ABI（`och.h`），和 `och` 命令行用同一套库。
- **MATLAB 包** `+omnicore`：在网关之上提供 `Device`、`RxStream`、`TxStream` 三个句柄类，以及 `Receiver`、`Transmitter` 两个 System object（Simulink 中作为 MATLAB System 模块使用）。

支持 MATLAB R2018a 及以后版本。代码不用 `arguments` 块，也不用 `Name=Value` 写法；R2021a 起你自己的代码当然可以用 `Name=Value`。设计和验收记录见仓库根目录的 `MEX_PLAN.md`。

| 状态 | 内容 |
|---|---|
| 已实机验证（macOS，MATLAB R2024b，Apple Silicon） | 设备与射频参数、单次采集与连续接收（含丢样报告）、突发 / 定时 / 循环发射、全双工、System object、Simulink、8bit 2T2R 61.44 MSPS、内部 PPS 对时 |
| 已实现、待实机验证 | Windows（MSVC 编译的 MEX）、Linux、外部 10 MHz + PPS、Ctrl+C 打断长调用（`OCH_MATLAB_CTRL_C`）、R2018a 实际加载 |
| 不支持 | 多板同步、FIR 装载、时钟链手动配置（用 `och` 命令行）；Simulink 代码生成 |

## 安装

发布页提供每个平台一个 zip：`omnicore-matlab-<版本>-<平台>.zip`，平台为 `win64`、`maca64`（Apple Silicon，MATLAB R2023b+）或 `glnxa64`。解压得到文件夹 `omnicore-matlab`，里面有 MEX 文件、`+omnicore` 包、OmniCore Host 运行库和示例，不需要另装 OmniCore Host。

```matlab
addpath('<解压目录>/omnicore-matlab');   % 用 savepath 永久保存
omnicore.version()                      % Library、Header 版本一致
omnicore.findDevices()                  % 列出已连接的设备
```

- **Windows**：USB 驱动仍需安装一次：装 OmniCore Host 安装包并运行 `och-driver install`。安装包里勾选 “Install the MATLAB toolbox” 时，同样的文件夹会装到 `C:\Program Files\OmniCore Host\matlab`，`addpath` 这个目录即可。
- **macOS**：从浏览器下载的 zip 会被 Gatekeeper 隔离，MEX 文件会被拒绝加载。解压后运行一次：`xattr -dr com.apple.quarantine omnicore-matlab`。
- **Linux**：需要 udev 规则，见 [Linux 安装](INSTALL_LINUX.md)。发布包里的 `libomnicore.so.1` 静态链接了 libstdc++，以避开 MATLAB 自带的旧版 libstdc++（见[排错](#排错)）。

设备同一时间只能被一个进程打开：MATLAB 持有设备时，`och`、GNU Radio 等其他程序打不开它，反之亦然。

### 从源码编译

```bash
cmake -S . -B build -DOCH_BUILD_MATLAB=ON              # 多个 MATLAB 时加 -DMatlab_ROOT_DIR=<MATLAB 目录>
cmake --build build -j
```

`build/bindings/matlab/toolbox` 可以直接 `addpath`。`cmake --build build --target omnicore_matlab_zip` 生成和发布包相同的 zip。已安装 OmniCore Host 时，可以只编 MATLAB 部分：`cmake -S bindings/matlab -B build-matlab -DCMAKE_PREFIX_PATH=<OCH 安装目录>`。

没有 CMake 时，在 MATLAB 里用 `mex` 编译（先 `mex -setup C++` 配好编译器；Windows 用 MSVC，需要安装包自带的 `lib\omnicore.lib`）：

```matlab
addpath('<仓库>/bindings/matlab');
build_omnicore_mex('<OCH 安装目录>', 'OutputDir', 'omnicore-matlab');
```

`mex` 只认它发布时已有的 Visual Studio 版本：比如 R2023b 不认 Visual Studio 2026，会报 “Supported compiler not detected”。这时装一个 Visual Studio 2022（Build Tools 即可），或者换用更新的 MATLAB。发布包的 MEX 用 R2023b + Visual Studio 2022 编译。

## 快速上手

```matlab
dev = omnicore.Device();                          % 只连了一台时自动选择
configure(dev, 'SampleRate', 30.72e6, 'RxFrequency', 2.45e9, 'RxBandwidth', 20e6);
dev.RxGainMode = 'manual';
dev.RxGain = [40 40];

[x, meta] = receive(dev, 1e6);                    % 1e6 x 2 complex single，±1
if meta.Overflow, disp(meta.Gaps), end

delete(dev)                                       % 或 clear dev：关闭设备
```

`examples/` 里有完整示例：

| 文件 | 内容 |
|---|---|
| `rx_spectrum.m` | 采集 1e6 样点，画 RX1 频谱 |
| `tx_tone_repeat.m` | 后台循环发射单音 10 s |
| `duplex_system_objects.m` | `Receiver` + `Transmitter` 帧式全双工，运行中改频率 |
| `timed_burst_loopback.m` | 定时 TX 突发 + 定时 RX，测回环延迟（需要 TX1 → 衰减器 → RX1） |
| `simulink_duplex_example.m` | 用脚本搭建并运行 Simulink 全双工模型 |

## 设备

```matlab
dev = omnicore.Device();                          % 唯一一台
dev = omnicore.Device('OmniCore62AF4C85');        % 指定序列号
dev = omnicore.Device('IQBits', 8);               % 打开时就设好参数
```

打开时先套用 OmniCore Host 默认配置（30.72 MSPS、1 GHz、RX `[1 2]`、无 TX、AGC slow），再叠加构造参数，一次下发。同一个 MATLAB 会话里再次打开同一台设备，会得到指向同一设备的第二个对象，最后一个对象删除时才关闭设备。

**配置属性**（每次赋值下发一次配置；读回的是设备实际值）：

| 属性 | 范围 |
|---|---|
| `SampleRate` | 2.5e6 – 61.44e6 Hz，RX/TX 共用。时钟链做不到的速率取最近的整 Hz 值，读回属性可得实际值 |
| `RxFrequency`、`TxFrequency` | 70e6 – 6e9 Hz |
| `RxBandwidth`、`TxBandwidth` | 200e3 – 56e6 Hz |
| `RxGain` | dB，1×2（标量设两个通道）。下限 −3；上限随 RX 本振频段：≤ 1.3 GHz 为 73，≤ 4 GHz 为 71，更高为 62。超过时压到上限并给 `omnicore:GainLimited` 警告；改频率后原增益超限也一样 |
| `RxGainMode` | `'manual'`、`'fast'`、`'slow'`、`'hybrid'`；`RxGain` 只在 manual 下起作用 |
| `TxGain` | dB，1×2，0 – 89.75，步进 0.25，等于 89.75 − 衰减（与 SoapySDR 插件、Pluto 一致）。只读的 `TxAttenuation` 给出对应衰减 |
| `RxChannels`、`TxChannels` | `1`、`2`、`[1 2]` 或 `[]`（关闭该方向）。有流时不能改 |
| `IQBits` | 12（默认）或 8。8bit 时 RX、TX 都必须是 `[1 2]`，见 [8bit USB IQ](iq8.md) |

超出范围的值报 `omnicore:InvalidArgument`，配置保持不变。同时改多个参数用 `configure(dev, Name, Value, ...)`：只下发一次，改采样率和带宽时流只暂停一次。

**状态与板级设置**：`info(dev)`（序列号、固件、USB 速度、通道数）、`status(dev)`（温度、参考 / PPS / VCXO 锁定、时间是否有效、RSSI）。属性 `ClockSource`、`TimeSource`、`RxLOSource`、`TxLOSource`、`PpsEnable`、`RssiEnable`、`RgbEnable`、`FanEnable`、`RxDCCorrection`、`RxIQCorrection`。

## 接收

### 一次性采集

```matlab
[x, meta] = receive(dev, N);
[x, meta] = receive(dev, N, 'OutputDataType', 'int16', 'StartTime', t0 + 0.5);
```

`x` 是 N × `numel(dev.RxChannels)` 的复数数组，每个通道一列：

| `OutputDataType` | 类型与范围 | 每样点每通道 |
|---|---|---|
| `'single'`（默认） | complex single，±1 | 8 字节 |
| `'double'` | complex double，±1 | 16 字节 |
| `'int16'` | complex int16，12bit 样点左对齐（低 4 位为 0） | 4 字节 |
| `'int8'` | complex int8，只在 `IQBits = 8` 时可用 | 2 字节 |

内存按此估算：1e8 样点双通道 single 为 1.6 GB，int16 为 0.8 GB。其他选项：`'Timeout'`（等待每块数据的秒数，默认 1）、`'BufferBytes'`（主机环形缓冲，默认 64 MiB）。

`meta` 字段：

| 字段 | 含义 |
|---|---|
| `Time` | 第一个样点的硬件时间，秒（未知时为 NaN） |
| `TimeTicks` | 同上，`uint64` 样点数，不丢精度 |
| `Overflow` | 是否丢过样点 |
| `Gaps` | K × 2：`[丢样后第一个样点的行号, 丢失样点数]`；数量未知时为 NaN |
| `Late` | 定时开始时，开始时间已经过去 |
| `Reconfigured` | 期间设备改过参数 |

丢失的样点不补零：`x` 只包含实际收到的样点，`receive` 丢样时返回的行数少于 N。丢失数量由每块数据头里的硬件时间算出，是精确值。

### 连续接收

```matlab
rx = rxStream(dev, 'OutputDataType', 'single', 'BufferBytes', 256*2^20);
start(rx);                                 % 或 start(rx, 'StartTime', t)
for k = 1:1000
    [x, meta] = read(rx, 65536);           % 每次 65536 × 通道数
end
stop(rx);
```

两次 `read` 之间，设备继续把数据写进主机环形缓冲；MATLAB 处理跟不上、缓冲满了才会丢样，丢在两次 `read` 之间的样点记在下一次的 `meta.Gaps` 第 1 行。缓冲按线路格式存（12bit 每个复样点 3 字节，8bit 为 2 字节）：64 MiB 在双通道 30.72 MSPS 下约 0.36 s。高速率、处理又慢时，建议用 `receive` 一次采完再处理，或加大 `BufferBytes`。

`read` 等不满 N 个样点时返回已收到的部分；一个都没等到则报 `omnicore:TIMEOUT`（等待时间由 `rx.Timeout` 决定）。

每台设备只有一个 RX 流：`RxStream` 运行时，`receive` 和第二个流会报 `omnicore:BUSY`。

## 发射

TX 的输入是 N × `numel(dev.TxChannels)` 的数组：single / double 复数按 ±1 缩放（超出部分限幅，给 `omnicore:TxClipped` 警告；实数视为 Q = 0），int16 为 12bit 左对齐，int8 只在 8bit 模式下可用。单通道时行向量也可以。

```matlab
dev = omnicore.Device('TxChannels', 1, 'TxFrequency', 2.45e9, 'TxGain', 40);
x = single(0.5 * exp(1j*2*pi*1e6/dev.SampleRate*(0:30719).'));

s = transmit(dev, x);                          % 一个突发；设备报告突发结束后返回
s = transmit(dev, x, 'StartTime', time(dev) + 0.5);   % 定时突发

transmitRepeat(dev, x);                        % 后台循环发射，立即返回
...                                            % MATLAB 可以做别的事
s = stopTransmit(dev);

tx = txStream(dev);                            % 分段写入
write(tx, x1, 'StartTime', t);                 % 带时间的写入开始一个新突发
write(tx, x2);
write(tx, x3, 'EndBurst', true);
s = status(tx);
stop(tx);
```

`transmit`、`stopTransmit` 和 `status` 返回的事件统计：

| 字段 | 含义 |
|---|---|
| `Underflow` | 设备发射时样点用完（射频欠流） |
| `Starved` | 突发进行中主机队列空了。定时突发等待开始时间时也会出现，此时无害 |
| `Late` | 定时突发的开始时间已经过去（`transmit` 另给 `omnicore:TxLate` 警告） |
| `BurstAck`、`AckTime`、`AckTicks` | 设备确认的突发结束次数，以及每次确认的硬件时间 |
| `Errors`、`LastError`、`Dropped` | 设备报告的 TX 错误、最后一条错误信息、事件队列满时丢掉的事件数 |

设备在队列里至少有一个 USB 传输（单通道 65472、双通道 32736 样点）或一个突发结束后才开始发射。分段写入时，在一个突发之内要以不低于采样率的速度喂数据，否则会欠流。改 TX 本振时 core 会暂停 TX 并丢弃已排队的样点；改 TX 增益不会。

每台设备只有一个 TX 流：`transmitRepeat` 或 `TxStream` 活动时，`transmit` 会报 `omnicore:BUSY`。

### 全双工时的流启停

RX、TX 可以同时运行，但有一条 core 的限制：任何一个流停下之后，只有在**两个方向都停止**时才能再开新流（core 要在两边都停时复位 USB 管道）。所以 TX 在发时，RX 可以开始一次，却不能停了再开，反过来也一样；违反时报 `omnicore:BUSY`（“stop both directions before reopening a stream”）。实际用法：

- 全双工测量用一个一直运行的 `RxStream`（或 `Receiver`）配一个 `TxStream`（或 `Transmitter`），不要每次测量都调用 `receive`。`examples/timed_burst_loopback.m` 就是这样在运行着的 RX 流里按时间戳找到每个定时突发。
- 需要换一种方式时，先停掉两个方向（`stop`、`stopTransmit`、`release`）。

## System object 与 Simulink

```matlab
dev = omnicore.Device('RxChannels', 1, 'TxChannels', 1, 'RxFrequency', 2.45e9, 'TxFrequency', 2.45e9);
rx = omnicore.Receiver('Device', dev, 'SamplesPerFrame', 30720);
tx = omnicore.Transmitter('Device', dev, 'Gain', 30);
for k = 1:1000
    [y, valid, overflow] = rx();
    underflow = tx(process(y));
end
release(rx); release(tx);
```

- `Receiver`：`[y, valid, overflow] = rx()`。属性 `SamplesPerFrame`、`OutputDataType`、`ChannelMapping`、`SampleRate`、`BufferBytes`、`Timeout`；可调属性 `CenterFrequency`、`Gain`、`GainMode`。一帧没收满时 `valid` 为 false，缺的部分补零。
- `Transmitter`：`underflow = tx(x)`。属性 `ChannelMapping`、`SampleRate`、`Latency`、`BufferBytes`、`Timeout`；可调属性 `CenterFrequency`、`Gain`。各帧连起来是一个不断开的突发。
- 数值属性为 `NaN`（`GainMode` 为 `''`）时保持设备当前值。可调属性的修改在**下一次调用**时生效，不中断接收。
- 不传 `Device` 时按 `SerialNumber` 自己打开设备（同一序列号的对象共用一台设备），`release` 时关闭；传入的 `Device` 不会被关闭。
- **全双工**：通道在有流时不能改，所以要么把同一个 `Device`（打开时设好 `RxChannels` 和 `TxChannels`）传给两个对象，要么先 `setup(rx); setup(tx, x)` 再开始调用。Simulink 会在仿真开始前初始化所有模块，所以两个模块用同一 `SerialNumber` 就能直接全双工。
- **`Latency`**：`Transmitter` 第一次调用时先发这么长的零（默认 0.1 s），让设备落后于写入。和 RX 同速、一帧对一帧地发送时，这就是 MATLAB 可以停顿多久而不欠流的余量。

**Simulink**：添加 *MATLAB System* 模块，System object 名填 `omnicore.Receiver` 或 `omnicore.Transmitter`，以“解释执行”方式运行（不支持代码生成）。`SampleRate` 设定时，`Receiver` 的采样时间为 `SamplesPerFrame / SampleRate`；仿真速度由 `Receiver` 等待每一帧决定，也就是实时运行。`examples/simulink_duplex_example.m` 用脚本搭好一个全双工模型并运行；`save_system('omnicore_duplex')` 可以保存下来。

已知现象：

- 启动 MATLAB 后第一次运行 Simulink 模型时，编译会造成一次超过 0.2 s 的停顿，`Transmitter` 可能报一次欠流；之后的运行没有。
- R2024b 对两个 mixin（`Propagates`、`SampleTime`）给出“已不推荐”警告；为了支持 R2018a 保留。
- Simulink 不允许字符参数可调，所以 `GainMode` 在 Simulink 里只能在仿真前设定。

## 硬件时间与 PPS

硬件时间是 64 位样点计数（1 tick = 1 样点），详见 [硬件时间戳](timestamps.md)。

```matlab
t = time(dev);              % 秒；timeTicks(dev) 为 uint64 样点数
dev.TimeSource = 'external';   % 'none'、'external'（PPS 输入）、'internal'（板上 PPS）、'sync'
setTime(dev, 0, 'AtNextPPS', true);     % 下一个 PPS 沿的样点时间为 0
[tp, ticks] = lastPPS(dev);             % 最近一个 PPS 沿的时间
dev.ClockSource = 'external';           % 外部 10 MHz 参考，见 [参考时钟](reference-lo.md)
```

- `setTime` 之后，`receive`、`transmit`、`write`、`start` 的 `'StartTime'` 都按这个时间计；整秒的 `StartTime` 落在 PPS 沿上。
- PPS 和采样时钟不同步，所以 PPS 沿锁存的时间有 ±1 样点抖动：对时后 `lastPPS` 可能读到整秒减 1。
- 改采样率会让硬件时间重新开始。时间设过时，给 `omnicore:TimeInvalidated` 警告。硬件时间的“有效”状态在关闭、重新打开设备后仍然保留。

**事件**：`e = events(dev)` 取出设备事件（最多保留最近 256 条），字段为 `Type`、`Message`、`HwTime`、`HwTicks`、`Count`、`HostTime`。USB 传输错误还会在下一次调用返回时，合并成一条 `omnicore:Transport` 警告。

## 错误、中断与卸载

- 错误标识统一为 `omnicore:<原因>`：`BUSY`（设备或流被占用）、`NOT_FOUND`、`TIMEOUT`、`InvalidArgument`、`InvalidHandle`（对象对应的设备已关闭）、`IO` 等，消息是 OmniCore Host 的错误说明。可以用 `try ... catch err, switch err.identifier` 处理。
- **Ctrl+C**：MATLAB 只在两次 MEX 调用之间响应 Ctrl+C，所以一次很长的 `receive` 要等它返回。打断后对象仍然完好，`stop(rx)` 或 `delete(dev)` 即可。用 CMake 选项 `-DOCH_MATLAB_CTRL_C=ON`（或 `build_omnicore_mex(..., 'CtrlC', true)`）编译时，长调用在 0.1 s 内停下并报 `omnicore:Interrupted`。这个选项用了 MATLAB 未公开的函数，所以默认不开。
- 有设备打开时，MEX 文件会锁住，`clear mex` 不会卸载它；`clear all` 会先删除对象、关闭设备，然后才卸载。退出 MATLAB 时会自动关闭所有设备。
- 对象在 `delete` 时关闭设备或流；出错只给警告，不报错。

## 排错

| 现象 | 原因与处理 |
|---|---|
| `omnicore:BUSY`，`device already owned by another OmniCore Host process` | 其他程序（`och`、GNU Radio、另一个 MATLAB）开着设备 |
| `omnicore:NOT_FOUND` | 没连设备，或驱动 / udev 规则没装好；先用 `och list` 检查 |
| Windows：`omnicore:LibraryNotFound` | MEX 旁边缺 `omnicore.dll` / `libusbK.dll`，或者它们没在 PATH 上 |
| macOS：MEX 文件“无法打开，因为无法验证开发者” | 运行 `xattr -dr com.apple.quarantine omnicore-matlab` |
| Linux：`GLIBCXX_3.4.xx not found` | `libomnicore` 是用较新的 GCC 编的，而 MATLAB 先加载了自带的旧 libstdc++。发布包已静态链接 libstdc++；自己编译时加 `-DOCH_STATIC_LIBSTDCXX=ON`，或者临时用 `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6 matlab` 启动 |
| 连续接收时 `meta.Overflow` 频繁 | MATLAB 处理太慢：加大 `BufferBytes`、减少每帧处理，或改用 `receive` 一次采完 |
| `Transmitter` 偶尔欠流 | 加大 `Latency`；不要在循环里画图或打印 |
| 改了 `RxChannels` 报 `omnicore:BUSY` | 先 `stop` 或 `release` 所有流 |
