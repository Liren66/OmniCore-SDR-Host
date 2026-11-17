# SoapySDR Plugin (GNU Radio, SDR++, SDRangel, Python)

`bindings/soapy` is the SoapySDR module for OmniCoreSDR, with driver name `omnicore`. Once installed, any software that supports SoapySDR can use the board directly: GNU Radio (`gr-soapy`, built in since 3.9; GNU Radio 4 also ships Soapy blocks), SDR++, SDRangel, CubicSDR, Pothos, and the SoapySDR Python / C++ APIs.

The plugin only uses the public C ABI (`och.h`), does not depend on OCH internal code, and shares the parameter mapping, interleaving, and time conversion in `bindings/common/` with the MATLAB toolbox.

| Status | Details |
|---|---|
| Verified on hardware (macOS, SoapySDR 0.8.1, GNU Radio 3.10) | Device discovery and arguments; RX single/dual channel (CS16/CF32), overflows and timestamps, timed and fixed-length RX; TX continuous and burst sending (including timed bursts and 1-sample short bursts), `readStreamStatus`; 2T2R full duplex (12-bit 30.72 MSPS, 8-bit 61.44 MSPS); changing LO / gain / sample rate while running; GNU Radio flowgraph start/stop 20 times, `rx_time` tags |
| Implemented, awaiting hardware verification | Linux, Windows (only built in CI); time alignment with external 10 MHz + PPS; checking channel mapping with a loopback cable; GQRX, SDR++, SDRangel |
| Not supported | Multi-board sync orchestration (`och sync`), FIR loading, manual clock chain configuration (use the `och` command line) |

Acceptance scripts are in `bindings/soapy/hwtest/`: `rx_accept.py`, `tx_accept.py`, `duplex_accept.py` (no loopback cable), `gr_rx_time.py`, `grc_run.py`.

## Building and installing

The plugin is a C++ module and must be built with **the same SoapySDR version and the same compiler** as the SoapySDR on your machine, so it is currently provided as source. SoapySDR 0.8 or newer is required.

**Linux** (Debian/Ubuntu as an example):

```bash
sudo apt-get install libsoapysdr-dev soapysdr-tools
cmake -S . -B build -DOCH_BUILD_SOAPY=ON
cmake --build build -j
sudo cmake --install build --prefix /usr/local
```

The plugin installs to `/usr/local/lib/SoapySDR/modules<version>/libomnicoreSupport.so`. If the system SoapySDR is not under `/usr/local`, it will not look for plugins in that directory either; in that case add the directory to the environment variable:

```bash
export SOAPY_SDR_PLUGIN_PATH=/usr/local/lib/SoapySDR/modules0.8
```

**macOS** (Homebrew):

```bash
brew install soapysdr libusb pkg-config
cmake -S . -B build -DOCH_BUILD_SOAPY=ON
cmake --build build -j
cmake --install build --prefix "$(brew --prefix)"
```

After Homebrew upgrades SoapySDR's minor version (e.g. 0.8 → 0.9), the plugin needs to be rebuilt.

**Plugin only** (OCH already installed, e.g. from the Linux package or Windows installer):

```bash
cmake -S bindings/soapy -B build-soapy -DCMAKE_PREFIX_PATH=/opt/omnicore/0.8.0
cmake --build build-soapy -j
```

**Windows** (not verified): the usual SoapySDR on Windows comes from radioconda / conda-forge or PothosSDR, both built with MSVC, so the plugin also has to be built with MSVC: in a conda environment with SoapySDR installed, point `CMAKE_PREFIX_PATH` at both the OCH install directory (the installer already includes the MSVC import libraries) and `%CONDA_PREFIX%\Library`, then put the resulting `omnicoreSupport.dll` into `%CONDA_PREFIX%\Library\lib\SoapySDR\modules0.8`. OCH's `bin` directory must be on PATH (the installer adds it by default).

Check that it is installed:

```bash
SoapySDRUtil --info                       # omnicore should be listed under Available factories
SoapySDRUtil --find="driver=omnicore"
SoapySDRUtil --probe="driver=omnicore"
```

## Device arguments

The device string is written as `driver=omnicore,key=value,...`; in GNU Radio, fill it into the Device parameter and Device Arguments.

| Argument | Values | Description |
|---|---|---|
| `serial` | Serial number | Selects one board when there are several; without it, the device can only be opened when exactly one board is connected |
| `rx_mask`, `tx_mask` | 0–3 | Set the channels of both directions at open (1 = channel 0, 2 = channel 1, 3 = both). **Required for GNU Radio full duplex**, see below |
| `iq_bits` | 12 / 8 | USB sample width; 8 requires both directions to be dual-channel |
| `clock_source` | `internal` / `external` | 10 MHz reference |
| `time_source` | `none` / `external` / `internal` / `sync` | PPS time source |

OCH holds a cross-process exclusive lock on the device: while the plugin has the device open, the `och` command line cannot open the same board. Several Soapy blocks in the same program (e.g. GNU Radio's source and sink) share one device instance and are not affected.

## RF parameters

| Soapy interface | Range | Sharing |
|---|---|---|
| Sample rate | 2.5 – 61.44 MSPS | One rate shared by RX, TX, and both channels; rates that cannot be hit exactly are replaced by the nearest whole-Hz rate with a notice in the log |
| Frequency (element `RF`) | 70 MHz – 6 GHz | Both channels of a direction share one LO |
| Bandwidth | 200 kHz – 56 MHz | Shared by both channels of a direction |
| RX gain (element `PGA`) | −3 dB to 73 dB (RX LO ≤ 1.3 GHz) / 71 dB (1.3–4 GHz) / 62 dB (> 4 GHz), step 1 | Independent per channel; `getGainRange` returns the upper limit for the current RX frequency, higher settings are clamped to it, and when the frequency changes, a gain above the new band's limit is lowered automatically (with a notice in the log) |
| TX gain (element `PGA`) | 0 … 89.75 dB, step 0.25 | Independent per channel; gain = 89.75 − attenuation (the same convention as PlutoSDR), **0 is minimum power** |
| RX AGC | On / off | When on, uses the mode set with `rx_agc_mode` (slow by default) |
| Automatic DC / IQ correction | On / off | Shared by both RX channels; needs firmware support |

The 2.5 MSPS lower limit on sample rate is conservative (without FIR decimation the AD9361's minimum is about 2.083 MSPS) and will be relaxed after hardware verification.

Changing only frequency or gain does not interrupt the data stream. Changing the sample rate or bandwidth pauses and resumes the stream, and reads/writes may return timeouts in the meantime; this is normal.

## Data formats

| Format | Description |
|---|---|
| `CS16` (native, full scale 32768) | 12-bit samples left-aligned to 16 bits |
| `CF32` | Normalized floating point |
| `CS8` (only with `iq_bits=8`, native) | 8-bit signed |

Soapy's `CS12` is not provided: the byte order of the device's 12-bit packed wire format differs from Soapy's definition.

Stream arguments (the args of `setupStream`, GNU Radio's Stream Arguments): `transfer_bytes` (USB transfer size, default 196608), `usb_queue_depth` (default 32), `host_buffer_bytes` (host buffer, default 16 MiB).

## Settings and sensors

Settings (`writeSetting` / `readSetting`; in GNU Radio they can also be changed with the `setting` command on the `cmd` message port):

| Key | Values | Description |
|---|---|---|
| `iq_bits` | 12 / 8 | Can only be changed while no data streams exist |
| `rx_agc_mode` | manual / fast / slow / hybrid | |
| `rx_lo_source`, `tx_lo_source` | internal / external | The external LO input frequency is 2× RF |
| `pps_enable` | true / false | PPS frequency monitoring (also starts the VCXO disciplining loop) |
| `rssi_enable` | true / false | The `rssi` sensor only has readings once this is on |
| `rgb_enable`, `fan_enable` | true / false | Status LED, fan |

Sensors: `temperature` (Zynq temperature), `ref_locked`, `pps_locked`, `vcxo_locked`, `time_valid`, `usb_speed`, and `rssi` for each RX channel (relative dB, uncalibrated). Items the firmware does not support read as `false` or `nan`.

## Hardware time

- Clock source: `internal` / `external` (10 MHz). Time source: `none` / `external` / `internal` / `sync`.
- `getHardwareTime("")` reads the current time, and `getHardwareTime("pps")` reads the time of the most recent PPS edge; `setHardwareTime(t)` sets it immediately, and `setHardwareTime(t, "pps")` takes effect on the next PPS edge.
- Every `readStream` carries `HAS_TIME` and the time of the first sample.
- **Changing the sample rate invalidates the hardware time**: the count keeps accumulating, but is converted at the new rate, so the time jumps. After changing the sample rate, call `setHardwareTime` again.

## Receive behavior

- When samples are lost (device overflow, host drops, or a timestamp discontinuity), `readStream` first returns `SOAPY_SDR_OVERFLOW` on its own, and the next call delivers the data with its time. GNU Radio re-tags `rx_time` after an overflow, so downstream time is always correct.
- A late timed RX likewise returns `SOAPY_SDR_TIME_ERROR` first.
- `activateStream(stream, HAS_TIME, t)` starts receiving at time t; `activateStream(stream, END_BURST, 0, n)` receives only n samples, and the last read carries `END_BURST`.

## Transmit behavior

- **When transmission starts**: `activateStream` only prepares; transmission actually starts once the written data fills one USB transfer (16368 samples per channel by default), or once a burst ending with `END_BURST` has been written. If you stop with less than one transfer of data and without writing `END_BURST`, that data is not sent, and a warning appears in the log.
- **Bursts**: Soapy has no "start of burst" flag; the next write after a burst ended with `END_BURST` starts a new burst. A write with `HAS_TIME` always starts a new burst, transmitted at the given time.
- **`END_BURST` alone without data** (`numElems = 0`): the plugin pads 1 zero sample to end the burst.
- **`readStreamStatus`**: a completed burst returns 0 with `END_BURST` and the time; a device underflow returns `SOAPY_SDR_UNDERFLOW`; a late timed burst returns `SOAPY_SDR_TIME_ERROR`. Normal idle time between bursts does not count as underflow.
- **Short bursts**: a very short first burst right after activation is fine (measured from 1 sample to 16 thousand samples, both timed and untimed are transmitted and acknowledged), so gr-soapy's length-tagged bursts can be used directly.

## GNU Radio

Use Soapy Custom Source / Sink (or `gnuradio.soapy.source` / `sink` in Python), with Device set to `driver=omnicore`.

- **Full duplex**: the source and sink must have the **same** `rx_mask` and `tx_mask` in Device Arguments, e.g. `rx_mask=1,tx_mask=1` for single-channel RX/TX, or `rx_mask=3,tx_mask=3` for dual channel. The reason is that GNU Radio starts one block and then the other; when the second block starts, the first is already running, and at that point the core does not allow the channel configuration to change.
- **Time tags**: the source emits `rx_time`, `rx_rate`, and `rx_freq` tags at start-up, after frequency and sample-rate changes, and after overflows.
- **Transmit**: the sink supports length-tagged bursts (`set_length_tag_name`), but gr-soapy **does not support timed transmit** (it does not read `tx_time`/`tx_sob`/`tx_eob` tags). When you need timed transmit, use the SoapySDR Python / C++ API directly.
- **Changing parameters while running**: changing the RX LO or gain loses no data. Changing the TX LO, bandwidth, or sample rate briefly pauses the device's transport, and in full duplex RX loses tens to one or two hundred milliseconds of data. The source reports every such gap and re-tags `rx_time`.
- **Start/stop**: stopping and restarting the whole flowgraph works. In full duplex, stopping only one direction and restarting it alone fails: the core requires both directions to be stopped before reopening (see [Architecture](architecture.md#explicit-tx-start-and-stop)). Software that can turn RX / TX on and off separately (such as SDRangel) is affected by this.

Example flowgraphs are in `examples/soapy/`.

## Python

```python
import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CF32

sdr = SoapySDR.Device("driver=omnicore")       # use a string; see the note below
sdr.setSampleRate(SOAPY_SDR_RX, 0, 10e6)
sdr.setFrequency(SOAPY_SDR_RX, 0, 1e9)
rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
buf = np.zeros(sdr.getStreamMTU(rx), np.complex64)
sdr.activateStream(rx)
sr = sdr.readStream(rx, [buf], len(buf))
print(sr.ret, sr.flags, sr.timeNs)
sdr.deactivateStream(rx)
sdr.closeStream(rx)
```

With the SoapySDR 0.8 Python bindings, passing arguments as a dict (`SoapySDR.Device(dict(driver="omnicore"))`) fails with `make() no match`; a string or `SoapySDR.SoapySDRKwargs` works.

For more complete usage, see the acceptance scripts in `bindings/soapy/hwtest/`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| omnicore not listed in `--info` | The plugin is not in SoapySDR's module directory: point `SOAPY_SDR_PLUGIN_PATH` at it, or check that the SoapySDR version matches the one used at build time |
| `--find` does not find the device | Confirm with `och find` first; on Linux check the udev rule, on Windows check `och-driver check` |
| Open fails with device busy | Another process (such as the `och` command line) is using the board |
| GNU Radio full duplex fails to start with `needs rx_mask=...` | Write the same `rx_mask` and `tx_mask` in the Device Arguments of both the source and the sink |
| Occasional overflows at dual-channel 61.44 MSPS | Test the same rate with `och bench` first; on macOS this setting has device-side overflows unrelated to the plugin |
| Time jumps after changing the sample rate | Expected; call `setHardwareTime` again |
| gr-soapy occasionally reports `Soapy sink error: TIMEOUT` | When changing sample rate or bandwidth, the device pauses transport for 0.05–0.7 s, and a few of these appear if it exceeds 100 ms; this is normal. If it keeps appearing without changing parameters, look at `OmniCore TX: write timed out ...` in the log: `started=0` means transmission has not started yet; `running=0` means the stream has stopped; `host_queue` staying full while `underflows` does not grow means the device is not taking data. Please report those log lines together with what you were doing at the time |
