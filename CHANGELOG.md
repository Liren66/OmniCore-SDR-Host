# Changelog

Features added in each release. For matching firmware versions see [Firmware Compatibility](docs/firmware-compatibility.md); for measured data see [Benchmarks](docs/benchmarks.md). The C ABI has been 1 throughout: new fields are always appended to the end of a struct or take over a previously reserved slot, and stay compatible with older callers through `struct_size`. New functions need a library that includes those exports.

## 0.8.0 (2026-11-13)

- SoapySDR plugin (`bindings/soapy`, driver name `omnicore`): usable directly from GNU Radio, SDR++, SDRangel, and SoapySDR Python / C++. Source builds only. See [SoapySDR Plugin](docs/soapy.md).
- MATLAB toolbox (`bindings/matlab`): `omnicore.Device`, `RxStream` / `TxStream`, `Receiver` / `Transmitter` System objects, and Simulink; one zip per platform, optionally installed by the Windows installer. See [MATLAB Toolbox](docs/matlab.md).
- Both share the parameter mapping and streaming logic in `bindings/common/` and only call the public C interface (enforced by the `bindings_use_public_api` test).

## 0.7.2 (2026-11-11)

- macOS 12+ support (universal binary, libusb statically linked); Linux and macOS share `src/transport/posix`.
- Core refactor: the transport layer is abstracted as `src/transport/link.hpp` (the `core_is_portable` test ensures the core has no platform code), and FakeLink offline tests were added.
- CI and release pipelines (Windows installer, Linux / macOS archives).

## 0.7.1 (2026-11-08)

- With bridge firmware 1.0.1, `och info` / `och power` read the USB-C state live every time (sending `typec?` on EP3 OUT).

## 0.7.0 (2026-11-07)

- Multi-board sync: `och sync` and `och_multi_sync()`, using a daisy chain or a common 10 MHz + PPS, with lock checks, AD9361 MCS, and loading a common time. Requires firmware with `sync=1`. See [Multi-Board Sync](docs/multi-board-sync.md).

## 0.6.2 (2026-11-07)

- `och info` shows the OmniCore SDR wordmark again; the README now uses real terminal screenshots.

## 0.6.1 (2026-11-07)

- `count` appended to the end of `och_event`: when the firmware merges underflow / GAP events during overload, one record stands for several device events.

## 0.6.0 (2026-11-07)

- Framed USB transport becomes the only format: every 16 KiB block carries a 16-byte header and a 64-bit sample time, and the number of samples lost to an overflow is derived exactly from the timestamp gap. Requires FPGA `CH32H417_IP` 1.3; older firmware can only use 0.5.x.
- Hardware timestamps, PPS time alignment, timed RX, timed TX bursts, and events stamped with sample time. See [Hardware Timestamps](docs/timestamps.md).

## 0.5.0 (2026-11-03)

- `och firmware` updates the CH32 bridge firmware over USB (requires bridge firmware 1.0.0+). See [Bridge Firmware Update](docs/firmware-update.md).

## 0.4.x (2026-10)

- 0.4.8: with PPS only, the CH32 PA4 DAC disciplines the 40 MHz oscillator (`clock.vcxo-status`). See [PPS](docs/pps.md).
- 0.4.7: RX RSSI query and on/off switch (`och rssi`); control UART raised to 3.125 MBd.
- 0.4.6: fast tuning: changing only the LO, RX gain, or TX attenuation no longer pauses the stream.
- 0.4.5: license set to GPL-3.0-or-later.
- 0.4.4: Linux libusb backend; sample rates the BBPLL cannot hit exactly are replaced by the nearest whole-Hz rate.
- 0.4.3: measured USB2 settings and CLI reference values.
- 0.4.2: layered terminal output; USB 2.0 High-Speed support.
- 0.4.0: optional 8-bit USB IQ for 2TX2RX. See [8-bit USB IQ](docs/iq8.md).

## 0.3.x

- 0.3.6: compatible with firmware using either of the two RGB default brightness levels (15% / 25%).
- 0.3.5: RGB status LED, temperature-controlled fan, and Zynq temperature. See [Board Control](docs/board-control.md).
- 0.3.4: PPS frequency measurement shown as a ±1 ppm LOCK.
- 0.3.3: PPS frequency monitoring.
- 0.3.2: ADF4002 external 10 MHz reference and independent external LO for RX / TX. See [Reference Clock and External LO](docs/reference-lo.md).
- 0.3.1: switches for RX DC and IQ tracking correction.
- 0.3: runtime reconfiguration: device-coordinated pause, apply, and resume, with stream handles kept valid.

## 0.2.x

- 0.2.1: USB-C power snapshot at startup (`och power`).
- 0.2: AD9361 hardware FIR loading and enable/disable, six-stage clock chain configuration. See [FIR and Clocks](docs/fir.md).
