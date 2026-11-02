# 桥接固件升级（0.5.0）

OCH 0.5.0 起可以通过 USB 升级 CH32H417 桥接固件，不需要 WCH-Link，也不依赖 Zynq SDK。
升级包 `OmniCore-Bridge-<版本>.ocfw` 随 [OmniCore-SDR-Firmware Releases](https://github.com/Liren66/OmniCore-SDR-Firmware/releases) 发布。

## 前提

- 设备上的桥接固件为 **1.0.0 及以上**（带 Bootloader）。更早的固件 `och firmware info` 显示 `legacy`，
  需先用 WCH-Link 烧录一次 Firmware Release 中的 `Merge.bin`。
- 没有其他程序打开设备（升级与 `och_open` 使用同一把设备锁，占用时返回 BUSY）。
- Windows 沿用已安装的 libusbK 驱动，Linux 沿用 udev 规则：Bootloader 与应用的 VID/PID、序列号和端点相同。

## 命令

| 命令 | 含义 |
|---|---|
| `och firmware` / `och firmware info` | 模式（application / bootloader / legacy）、版本、Bootloader 版本、构建号 |
| `och firmware check FILE.ocfw` | 离线校验升级包（头、硬件 ID、CRC），显示版本 |
| `och firmware update FILE.ocfw` | 进入 Bootloader → 擦除 → 写入 → 校验 → 重启，确认新版本运行 |
| `och firmware boot` | 离开 Bootloader，启动已安装的应用 |
| `och firmware enter` | 重启进入 Bootloader（恢复或调试用） |

`och info` 额外显示桥接固件版本；设备处于 Bootloader 时，`och info` 和数据流命令直接提示用 `och firmware`。
所有命令支持 `--serial` 和 `--json`。

## 中断与恢复

Bootloader 在整包 CRC 通过后才写入应用第 0 页和应用头。升级中途断电、拔线或进程被结束时，
设备重启后停留在 Bootloader（`application: missing`），再次执行 `och firmware update` 即可。
Bootloader 只擦写应用区和标志扇区，stage-0 与 Bootloader 自身不会被 USB 升级改动。

Bootloader 优先使用 USB3，失败时退回 USB2。若 10 秒内没有主机完成枚举且应用完好，它自动返回应用；
在 Bootloader 中卡死时，看门狗复位后也回到应用。

## C API

```c
och_firmware_info fw = { sizeof fw };
och_firmware_query(NULL, &fw);                       /* 不打开设备，不访问 SDK */
och_firmware_update(NULL, "OmniCore-Bridge-1.0.0.ocfw", progress_cb, user);
```

`och_firmware_read_image` 校验文件，`och_firmware_enter_bootloader` / `och_firmware_start_app` 切换模式，
`och_get_firmware_info` 返回已打开设备在 `och_open` 时读取的桥接固件信息。版本按
`major<<16 | minor<<8 | patch` 编码。进度回调的阶段为 `OCH_FW_STAGE_*`，写入阶段的 done/total 为字节数。

设备侧协议（厂商请求 `0xB0`–`0xB5`、Flash 布局、`.ocfw` 格式）见 Firmware 仓库
`OmniCore-SDR-Bridge/README.md` 与 `Common/ocfw.h`。
