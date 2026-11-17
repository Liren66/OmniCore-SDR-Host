# Firmware and Protocol Dependencies

The host installer only installs the Windows programs, the host C/C++ development interface, and the libusbK driver for the target data interface. It contains no copy of the board SDK source, ELF, BOOT.bin, or FPGA bitstream, and never flashes the CH32, SDK, FPGA, or boot flash automatically. After installing the software on a new computer, you still need to connect an OmniCoreSDR with matching firmware.

## Version mapping

Since OCH 0.6.0, only the framed USB transport is supported (each 16 KiB block carries a 16 B header and a sample time), which requires Zynq firmware [v2026.11.07](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07) or later. If `system_info?` does not contain `transport=framed16k` when the device is opened, `och_open` returns `OCH_UNSUPPORTED` and asks you to load FPGA `CH32H417_IP` 1.3 and `SDK_CMD_1.1`.

The current release, OCH 0.8.0, recommends firmware [v2026.11.08](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.08) (Zynq `SDK_CMD_1.2_20261107_SYNC` + CH32 bridge 1.0.1). 0.7.2 and 0.8.0 add no new firmware requirements: 0.7.2 added macOS support and refactored the core, and 0.8.0 added the SoapySDR plugin and the MATLAB toolbox, both of which only call the public C interface.

| OCH | Zynq firmware (BOOT.bin) | FPGA `CH32H417_IP` | SDK identifier | CH32 bridge |
|---|---|---|---|---|
| **0.8.0** | Same as 0.7.1; v2026.11.08 recommended | 1.3 (`0x01030001`) | `SDK_CMD_1.2_20261107_SYNC` | **1.0.1** (1.0.0 still works) |
| 0.7.2 | Same as 0.7.1 | 1.3 (`0x01030001`) | `SDK_CMD_1.2_20261107_SYNC` | 1.0.1 (1.0.0 still works) |
| 0.7.1 | Same as 0.7.0; reading the USB-C current every time needs CH32 bridge 1.0.1 ([v2026.11.08](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.08)) | 1.3 (`0x01030001`) | `SDK_CMD_1.2_20261107_SYNC` | **1.0.1** (1.0.0 still works, but the USB-C current can only be read the first time after power-up) |
| 0.7.0 | v2026.11.07 or later; `och sync` needs v2026.11.07.2 (SDK_CMD_1.2, FPGA with OC_SYNC_AXI 1.0) | 1.3 (`0x01030001`) | `SDK_CMD_1.2_20261107_SYNC` | 1.0.0 (unchanged) |
| 0.6.1 | v2026.11.07 or later; v2026.11.07.1 recommended | 1.3 (`0x01030001`) | `SDK_CMD_1.1_20261107_EVTCOUNT` (v2026.11.07.1) | 1.0.0 (unchanged) |
| 0.6.0 | v2026.11.07 or later | 1.3 (`0x01030001`) | `SDK_CMD_1.1_20261107_FRAMED` | 1.0.0 (unchanged) |
| 0.5.x | v2026.11.03 or earlier | 1.1 | `SDK_CMD_1.0_*` | 1.0.0 / legacy |

0.6.1 also works with v2026.11.07, except that under overload the firmware reports events one at a time without `count` (each counts as 1); v2026.11.07.1 merges overload events and includes `count=`. See [Hardware Timestamps](timestamps.md).

The two sides must match: 0.6.0 cannot connect to older firmware, and 0.5.x cannot stream IQ with newer firmware (the new firmware no longer sends the 16 KiB incrementing start header). The CH32 bridge firmware is transparent to the data contents, so 1.0.0 works with both combinations.

Since 0.5.0, CH32 bridge firmware 1.0.0 and later can be updated over USB by manually running `och firmware update FILE.ocfw`; see [Bridge Firmware Update](firmware-update.md). `och firmware info` shows the bridge firmware version; bridge firmware older than 1.0.0 is shown as legacy.

## Capability checks

OCH enables features based on the capability fields the firmware reports, not on firmware names: extensions such as FIR/clocks once kept an old firmware name (e.g. `SDK_CMD_1.0_20261023`), so the same name does not mean the same features. When a capability is missing, only the corresponding API / command returns `OCH_UNSUPPORTED` and everything else works as usual. Capability checks run before any stream is paused, so an unsupported request never interrupts existing streams.

| Component | Required capability | How it is checked |
|---|---|---|
| ZYNQ SDK | Command wrapper protocol v1 | host_protocol? contains host_protocol=1 |
| ZYNQ SDK | Framed USB transport (16 B block header, sample time, GAP) | system_info? contains transport=framed16k |
| ZYNQ SDK time extension | Time base, PPS time alignment, timed RX, events with time | Time APIs require hw_time=1 |
| ZYNQ SDK FIR extension | Safe chunked upload, snapshot read, dual-channel SPI verification | FIR features require system_info? to contain fir_upload=1 |
| ZYNQ SDK clock extension | Shared Fs without a reduced-rate fallback, and full-chain transactions | Safe clock settings require system_info? to contain clock_config=1 |
| ZYNQ SDK RX correction extension | RF/BBDC DC and RX quadrature tracking switches, status read-back | RX correction requires system_info? to contain rx_correction=1 |
| ZYNQ SDK ADF4002 extension | External 10 MHz disciplining, GPIO check, and D5 self-test | Status/self-test require adf4002=1; selecting the source also requires ref_source=1 |
| ZYNQ SDK multi-board sync extension | REF_OUT, TRIG, SYNC/PPS edge counting, AD9361 MCS steps, time_source=sync | Sync features require system_info? to contain sync=1 |
| ZYNQ SDK external LO extension | Independent RX/TX sources, verification, and nominal frequency state | LO source requires external_lo=1 |
| Optional 2TX2RX 8-bit extension | SDK format command and FPGA signed8 path | Only selecting 8-bit requires iq_format=1 iq8_2t2r=1; the default 12-bit stays compatible with older SDKs |
| CH32 | Existing diagnostic and stream cleanup protocol v4 | Diagnostic version from USB vendor request 0xD0 |
| FPGA/IP | `CH32H417_IP` 1.3: 16 KiB framed blocks, 64-bit sample time, event FIFO; packed12 / signed8 channel layout unchanged | SDK reports `fpga=CH32H417_IP_0x01030001`; the stream verifies the header of every block |
| USB | SuperSpeed or USB 2.0 High-Speed (480 Mbit/s); data endpoints 01/81, control endpoints 02/82 | Confirmed at enumeration and open; Full-Speed (12 Mbit/s) is not supported |
| Windows driver | libusbK 3.1.0.0 from the release package | och-driver check; actual installation is still subject to Windows policy |

Even if an older SDK supports the raw serial commands, it may lack the command wrapper protocol or the framed transport, so compatibility cannot be assumed. When the version requirements are not met, OCH fails explicitly, rather than mistaking a timeout or a leftover reply for a successful new command. See [Device Protocol](protocol.md) for the format of each command.

The board SDK, FPGA, and CH32 projects live in the [OmniCore-SDR-Firmware](https://github.com/Liren66/OmniCore-SDR-Firmware) repository, separate from OmniCore Host. The host installer does not update the Zynq / FPGA firmware. A new ELF running from RAM does not mean that version will still boot after a power cycle; for long-term use, deploy the boot image following the firmware repository's procedure. Settings such as custom FIR / clocks and the RX correction switches are kept only in the RAM of the running SDK and return to defaults after a power cycle or reloading the program.

Opening the device stops any previous USB session and clears its buffers, so do not let other host software, debug scripts, and OCH operate the same data interface at the same time. Use `och-driver check` when you only need to check the Windows driver, and `och find` when you only need to enumerate.

## Features that need several components updated together

| Feature | Firmware requirement | If not met |
|---|---|---|
| PPS disciplining of the 40 MHz (PA4 tuning hardware) | SDK `vcxo=1 pps_adjustment=1` + CH32 bridge firmware with the PA4 handshake | Older bridge firmware drives PA4 push-pull as an LED and does not answer the handshake, so the new SDK refuses to enable the ADF4002 charge pump (`error=-116`); setting external 10 MHz fails instead of conflicting with PA4. Older bridge firmware cannot be used in PPS-only mode either. `och_get_vcxo_status` returns `OCH_UNSUPPORTED` on older SDKs. See [PPS](pps.md) |
| Board RGB, fan, Zynq temperature | FPGA with BOARD_CTRL_AXI, SDK `board_rgb=1 fan_control=1 core_temp=1` | The corresponding commands are unsupported. The default brightness of both old and new SDKs (15% / 25%) is recognized; see [Board Control](board-control.md) |
| USB 2.0 High-Speed | CH32 bridge firmware with USBHS support | Older bridge firmware lacks the USBHS control endpoint and 0xD0 diagnostics / stream cleanup; only High-Speed with a 512 B max packet size is supported, and Full-Speed with 64 B is always rejected |
| RX RSSI switch | SDK `rssi=1` (`rssi_enable`, `rx_rssi?`) | Older SDKs have no switch: OCH reads the RSSI line from `system_status?` (about 190 ms, versus about 1.1 ms on new SDKs), `enabled` is treated as 1, and the switch commands return unsupported. For the meaning see [CLI: RX RSSI](cli.md#rx-rssi) |
| Control UART at 3.125 MBd | Zynq SDK and CH32 bridge firmware updated together | With one side new and the other old, `och find` still sees the device, but `och info` fails because `host_protocol?` times out. See [Device Protocol: Control UART](protocol.md#control-uart) |

On USB2, `och info` shows exactly **480 Mbit/s (High-Speed, USB 2.0)**; the 61.44 MSPS shown there is the RF sample-rate ceiling, not a rate USB2 can sustain. For USB2 settings see [Benchmarks](benchmarks.md#usb-20-high-speed).

## TX FIR interpolation ×2 / ×4

When the AD9361 driver writes TX FIR coefficients, it temporarily sets `0x002[1:0]` to the target interpolation, but the older SDK's `fir_verify_coeff` (`console_commands/sdr_fir.c`) reads them back in the current mode after the write. With the TX FIR off (bypassed), the read-back of a ×2 / ×4 filter does not match what was written, so the SDK reports a failure and rolls back, returning `ERR fir_or_clock_apply code=-1 rollback=verified`. As a result, older firmware cannot load a TX interpolation filter while it is off (including `och fir tx load`), and an enabled ×2 / ×4 TX FIR cannot be turned off.

The fixed SDK enters the ALERT state when reading back TX coefficients, temporarily sets `0x002[1:0]` to the configured interpolation, and restores the original value afterwards; every coefficient is still checked. The fix has been tested on hardware: loading TX ×2 / ×4 while off, enabling, and disabling all succeed, and nearest-sample-rate selection and full-duplex throughput tests pass for all combinations of RX / TX factors. RX decimation is not affected by this problem. The fix is only in the board SDK and the host program needs no changes; the firmware version string did not change, so confirm your firmware by whether it can load a TX interpolation filter.
