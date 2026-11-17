# Bridge Firmware Update

Since OCH 0.5.0, the CH32H417 bridge firmware can be updated over USB without a WCH-Link and without depending on the Zynq SDK.
The update package `OmniCore-Bridge-<version>.ocfw` is published with [OmniCore-SDR-Firmware Releases](https://github.com/Liren66/OmniCore-SDR-Firmware/releases).

## Prerequisites

- The bridge firmware on the device is **1.0.0 or later** (with a bootloader). For older firmware, `och firmware info` shows `legacy`,
  and you first need to flash `Merge.bin` from the firmware release once with a WCH-Link. See
  [Firmware README: Updating the CH32 bridge firmware](https://github.com/Liren66/OmniCore-SDR-Firmware#升级-ch32-桥接固件) for the steps.
- No other program has the device open (the update uses the same device lock as `och_open` and returns BUSY while the device is in use).
- Windows keeps using the installed libusbK driver and Linux the udev rule: the bootloader and the application share the same VID/PID, serial number, and endpoints.

## Commands

| Command | Meaning |
|---|---|
| `och firmware` / `och firmware info` | Mode (application / bootloader / legacy), version, bootloader version, build number |
| `och firmware check FILE.ocfw` | Verify an update package offline (header, hardware ID, CRC) and show its version |
| `och firmware update FILE.ocfw` | Enter bootloader → erase → write → verify → reboot, then confirm the new version is running (about 5 s on USB3, 10 s on USB2) |
| `och firmware boot` | Leave the bootloader and start the installed application |
| `och firmware enter` | Reboot into the bootloader (for recovery or debugging) |

`och info` additionally shows the bridge firmware version. While the device is in the bootloader, `och info` and the streaming commands tell you to use `och firmware`.
All commands accept `--serial` and `--json`.

## Interruption and recovery

The bootloader writes application page 0 and the application header only after the CRC of the whole package has passed. If power is lost, the cable is unplugged, or the process is killed during an update,
the device stays in the bootloader after restarting (`application: missing`); just run `och firmware update` again.
The bootloader only erases and writes the application area and the flag sector; stage-0 and the bootloader itself are never changed by a USB update.

The bootloader prefers USB3 and falls back to USB2 if that fails. If no host completes enumeration within 10 seconds and the application is intact, it returns to the application automatically;
if it hangs in the bootloader, the watchdog reset also returns to the application.

## C API

```c
och_firmware_info fw = { sizeof fw };
och_firmware_query(NULL, &fw);                       /* does not open the device or touch the SDK */
och_firmware_update(NULL, "OmniCore-Bridge-1.0.1.ocfw", progress_cb, user);
```

`och_firmware_read_image` verifies a file, `och_firmware_enter_bootloader` / `och_firmware_start_app` switch modes,
and `och_get_firmware_info` returns the bridge firmware information read at `och_open` for an open device. Versions are encoded as
`major<<16 | minor<<8 | patch`. The progress callback stages are `OCH_FW_STAGE_*`; during the write stage, done/total are in bytes.

For the device-side protocol (vendor requests `0xB0`–`0xB5`, flash layout, `.ocfw` format), see
`OmniCore-SDR-Bridge/README.md` and `Common/ocfw.h` in the Firmware repository.
