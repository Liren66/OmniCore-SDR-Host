# RGB、风扇温控与 Zynq 温度

板级 SDK 默认开启 RGB 状态指示，按 15% 通道强度输出（每个通道最大 38/255）；风扇自动温控默认关闭，PWM 为 0%。开关设置在当前板上程序运行期间保留，关闭/重新打开 OCH 不会重置它们，`rf_reinit!` 也保留设置。重新启动板上程序后恢复默认值。

## 命令行

| 功能 | OCH 命令 | SDK 命令 |
|---|---|---|
| 开启/关闭 RGB | `och set rgb.enabled on` / `off` | `rgb_enable=1` / `0` |
| RGB 开关查询 | `och get rgb.enabled` | `rgb_enable?` |
| RGB 状态查询 | `och get rgb.status` | `rgb_status?` |
| 开启/关闭自动温控 | `och set fan.enabled on` / `off` | `fan_enable=1` / `0` |
| 温控开关查询 | `och get fan.enabled` | `fan_enable?` |
| 风扇状态查询 | `och get fan.status` | `fan_status?` |
| 核心温度 | `och get core.temperature` | `core_temp?` |

开关也接受 `0/1`。所有命令支持 `--json`。`och get system.status` 同时包含 RGB、风扇、Zynq 温度以及原有的 AD9361 温度，温度行分别标注 `source=zynq`、`source=ad9361`。独立核心温度查询在终端中额外显示摄氏度 `Celsius`，风扇状态额外显示占空比百分数 `duty_pct`。

`och get system.info` 应包含 `board_rgb=1 fan_control=1 core_temp=1`。规范命令及 C/C++ 接口会先检查能力并验证响应；旧固件缺少能力时返回不支持。OCH 开关及状态查询不会停止 IQ 流，不改变射频配置或缓冲区。已有应用占用设备时，使用该应用内的 C/C++ 接口，不能通过另一个 OCH 进程操作它。

## RGB 状态

OCH 0.3.6 同时兼容当前 15% 与旧版 25% 固件，亮度按设备实际回报显示。下表为当前 15% 固件的颜色值。

| 状态 | 颜色 | R/G/B | 闪烁 |
|---|---|---|---|
| initializing | 黄色 | 38/38/0 | 常亮 |
| idle | 绿色 | 0/38/0 | 常亮 |
| rx：RX 上传开启 | 蓝色 | 0/0/38 | 常亮 |
| tx：TX 发射开启 | 紫色 | 38/0/38 | 常亮 |
| duplex：RX 上传与 TX 发射开启 | 青色 | 0/38/38 | 常亮 |
| recovering：桥接错误清理或 RX 恢复 | 橙色 | 38/10/0 | 1 Hz |
| fault：RF、数字接口或桥接初始化失败 | 红色 | 38/0/0 | 常亮 |
| overtemp：Zynq 高温告警 | 红色 | 38/0/0 | 4 Hz |
| off | 黑色 | 0/0/0 | 熄灭 |

RGB 关闭优先，其余优先级为高温、故障、异常恢复、初始化、收发、空闲。错误恢复后不再根据历史错误计数报警。RGB 状态中的颜色字段是当前闪烁相位的输出值。状态依据 SDK 运行状态，不表示光学检测、实际 RF 功率或 ADF4002 硬件锁定。15% 是通道数值缩放，不是经过光度校准的感知亮度。

## 温控策略

温控使用 Zynq XADC 内部温度，连续硬件转换、16 次平均，软件约每秒读取一次。有效温度以毫摄氏度传输；例如 `64500` 表示 64.5℃。温度无效时文本为 `unknown`，C API 数值置零但 `valid=0`，应用必须检查有效标志。超过 2.5 秒未获得有效软件采样，或 XADC 配置/读数异常时，温控开启会请求 100% PWM。

| 升温温度范围 | 高电平占空比 |
|---|---:|
| 小于 40℃ | 0% |
| 40～小于 50℃ | 30% |
| 50～小于 60℃ | 50% |
| 60～小于 70℃ | 70% |
| 70℃及以上 | 100% |

降档需低于对应门限 2℃，例如从 50% 降到 30% 要低于 48℃。从 0% 启动时先输出 100% 约 500ms，随后按目标占空比运行。温控关闭立即请求 0%，高温不会自动改变开关。高温告警在 80℃触发，低于 75℃解除；这些是本项目的软件设定值。

`duty_permille`、`target_permille` 范围为 0～1000，700 对应 70%。PWM 固定 25kHz，风扇引脚 D19，RGB 引脚 E19。占空比表示 PWM 控制脚高电平比例，0% 是否实际停转以及最低启动占空比取决于所接风扇；接口不提供转速反馈。服务在主循环和已有延时/串口等待中执行，不使用新的计时器中断。

## C/C++ 接口

新增接口，不改变现有 ABI 1 结构：

```c
och_rgb_status rgb = {0};
och_fan_status fan = {0};
och_core_temperature temp = {0};
rgb.struct_size = sizeof(rgb);
fan.struct_size = sizeof(fan);
temp.struct_size = sizeof(temp);

/* 检查每次调用的返回码；设备句柄由 och_open 获得。 */
int rc = och_get_core_temperature(device, &temp);
if (rc == OCH_OK && temp.valid) {
    double celsius = temp.milli_celsius / 1000.0;
    /* 使用 celsius。 */
}
rc = och_set_rgb_enabled(device, 1);
rc = och_set_fan_enabled(device, 1);
rc = och_get_rgb_status(device, &rgb);
rc = och_get_fan_status(device, &fan);
rc = och_set_fan_enabled(device, 0);
```

C++ 包装接口为 `device.rgb_status()`、`device.fan_status()`、`device.core_temperature()`、`device.set_rgb_enabled(bool)` 和 `device.set_fan_enabled(bool)`。设置操作验证回读；相同设置不重复写入。控制失败会返回错误，但不停止原有数据流。

主机安装包仅更新 OCH。板卡需要导出包含 BOARD_CTRL_AXI 的硬件，并加载支持板控的 SDK ELF 或 BOOT.bin；安装器不会下载板上固件。
