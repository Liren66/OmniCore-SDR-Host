# MATLAB Toolbox

`bindings/matlab` lets MATLAB use OmniCoreSDR directly, without going through SoapySDR and without depending on any toolbox. It has two parts:

- **MEX gateway** `omnicore_mex`: only calls the public C ABI (`och.h`) and uses the same library as the `och` command line.
- **MATLAB package** `+omnicore`: on top of the gateway, it provides three handle classes, `Device`, `RxStream`, and `TxStream`, plus two System objects, `Receiver` and `Transmitter` (used in Simulink as MATLAB System blocks).

MATLAB R2018a and later are supported. The code does not use `arguments` blocks or `Name=Value` syntax; your own code can of course use `Name=Value` from R2021a on. Tests are in `bindings/matlab/tests/` (`OfflineTest` needs no device; `*HardwareTest` needs one).

| Status | Details |
|---|---|
| Verified on hardware (macOS, MATLAB R2024b, Apple Silicon) | Device and RF parameters, one-shot capture and continuous receive (including drop reporting), burst / timed / looped transmit, full duplex, System objects, Simulink, 8-bit 2T2R 61.44 MSPS, internal PPS time alignment |
| Implemented, awaiting hardware verification | Windows (MSVC-built MEX), Linux, external 10 MHz + PPS, interrupting long calls with Ctrl+C (`OCH_MATLAB_CTRL_C`), actual loading on R2018a; running the application examples `fm_radio`, `fm_loopback`, `video_link`, and `sdr_scope` with a device (simulation mode verified) |
| Not supported | Multi-board sync, FIR loading, manual clock chain configuration (use the `och` command line); Simulink code generation |

## Installation

The releases page provides one zip per platform: `omnicore-matlab-<version>-<platform>.zip`, where the platform is `win64`, `maca64` (Apple Silicon, MATLAB R2023b+), or `glnxa64`. Extracting it gives the `omnicore-matlab` folder, containing the MEX file, the `+omnicore` package, the OmniCore Host runtime libraries, examples, and reference documentation; OmniCore Host does not need to be installed separately.

```matlab
addpath('<extract-dir>/omnicore-matlab');   % use savepath to keep it permanently
omnicore.version()                         % Library and Header versions match
omnicore.findDevices()                     % list connected devices
```

- **Windows**: the USB driver still needs to be installed once: install the OmniCore Host installer and run `och-driver install`. If you tick "Install the MATLAB toolbox" in the installer, the same folder is installed to `C:\Program Files\OmniCore Host\matlab`; just `addpath` that directory.
- **macOS**: Gatekeeper quarantines zips downloaded from a browser, and the MEX file will be refused. After extracting, run once: `xattr -dr com.apple.quarantine omnicore-matlab`.
- **Linux**: requires the udev rule; see [Installing on Linux and macOS](install.md). The `libomnicore.so.1` in the release package statically links libstdc++ to avoid the older libstdc++ shipped with MATLAB (see [Troubleshooting](#troubleshooting)).

A device can only be opened by one process at a time: while MATLAB holds the device, `och`, GNU Radio, and other programs cannot open it, and vice versa.

### Building from source

```bash
cmake -S . -B build -DOCH_BUILD_MATLAB=ON              # with several MATLABs, add -DMatlab_ROOT_DIR=<MATLAB-dir>
cmake --build build -j
```

You can `addpath` `build/bindings/matlab/toolbox` directly. `cmake --build build --target omnicore_matlab_zip` produces the same zip as the release package. With OmniCore Host already installed, you can build only the MATLAB part: `cmake -S bindings/matlab -B build-matlab -DCMAKE_PREFIX_PATH=<OCH-install-dir>`.

Without CMake, build with `mex` inside MATLAB (configure a compiler with `mex -setup C++` first; on Windows use MSVC, which needs the `lib\omnicore.lib` shipped with the installer):

```matlab
addpath('<repo>/bindings/matlab');
build_omnicore_mex('<OCH-install-dir>', 'OutputDir', 'omnicore-matlab');
```

`mex` only recognizes the Visual Studio versions that existed when it was released: for example, R2023b does not recognize Visual Studio 2026 and reports "Supported compiler not detected". In that case install Visual Studio 2022 (Build Tools is enough), or use a newer MATLAB. The release package's MEX is built with R2023b + Visual Studio 2022.

## Quick start

```matlab
dev = omnicore.Device();                          % selected automatically when only one is connected
configure(dev, 'SampleRate', 30.72e6, 'RxFrequency', 2.45e9, 'RxBandwidth', 20e6);
dev.RxGainMode = 'manual';
dev.RxGain = [40 40];

[x, meta] = receive(dev, 1e6);                    % 1e6 x 2 complex single, ±1
if meta.Overflow, disp(meta.Gaps), end

delete(dev)                                       % or clear dev: closes the device
```

## Reference documentation

The toolbox includes reference pages in MATLAB help format (`doc/`): syntax, description, examples, arguments, and tips for every class, function, and method, plus topic pages on sample formats, streams and full duplex, hardware time, Simulink, and errors and warnings.

```matlab
omnicore.doc                  % start page
omnicore.doc('receive')       % a specific function, method, or topic
help omnicore                 % list of package contents
```

When the toolbox folder is on the path, these pages also appear under "Supplemental Software" on the Help browser start page, registered by the `info.xml` in the folder.

## Examples

The application examples in `examples/` can all run without a device using `'Simulate', true` (a software-simulated channel: delay, attenuation, LO offset, noise, ADC clipping), using only base MATLAB:

| File | Contents |
|---|---|
| `fm_radio.m` | FM broadcast radio: offset tuning, polyphase decimation, phase-discriminator demodulation, de-emphasis, with live playback and RF and MPX spectrum displays |
| `fm_loopback.m` | FM transmit-and-receive loopback: full-duplex RX/TX in the same loop; at the end it aligns the transmitted and received audio and reports end-to-end latency and audio SNR |
| `video_link.m` | Digital video link: JPEG frames packed into QPSK bursts (Zadoff-Chu preamble, frequency offset estimation, pilot phase tracking, checksum), transmitted at scheduled hardware times and cut out of the RX stream for decoding. Defaults to 433.92 MHz, since over-the-air RX/TX at 2.4 GHz gets interrupted by Wi-Fi frames |
| `sdr_scope.m` | Simple oscilloscope and spectrum analyzer: edge trigger, I/Q / magnitude / phase, averaging and peak hold, with frequency, sample rate, and gain adjustable live from the UI |
| `rx_spectrum.m` | Capture 1e6 samples and plot the RX1 spectrum |
| `tx_tone_repeat.m` | Transmit a tone in a background loop for 10 s |
| `duplex_system_objects.m` | Framed full duplex with `Receiver` + `Transmitter`, changing frequency while running |
| `timed_burst_loopback.m` | Timed TX burst + timed RX to measure loopback latency (needs TX1 → attenuator → RX1) |
| `simulink_duplex_example.m` | Build and run a Simulink full-duplex model from a script |

## Device

```matlab
dev = omnicore.Device();                          % the only one
dev = omnicore.Device('OmniCore62AF4C85');        % by serial number
dev = omnicore.Device('IQBits', 8);               % set parameters at open
```

On open, the OmniCore Host default configuration (30.72 MSPS, 1 GHz, RX `[1 2]`, no TX, AGC slow) is applied first, then the constructor arguments are layered on top and sent in one go. Opening the same device again in the same MATLAB session gives a second object pointing to the same device; the device is only closed when the last object is deleted.

**Configuration properties** (each assignment sends one configuration; reading returns the device's actual value):

| Property | Range |
|---|---|
| `SampleRate` | 2.5e6 – 61.44e6 Hz, shared by RX/TX. Rates the clock chain cannot produce are set to the nearest whole-Hz value; read the property back for the actual value |
| `RxFrequency`, `TxFrequency` | 70e6 – 6e9 Hz |
| `RxBandwidth`, `TxBandwidth` | 200e3 – 56e6 Hz |
| `RxGain` | dB, 1×2 (a scalar sets both channels). Lower limit −3; the upper limit depends on the RX LO band: 73 at ≤ 1.3 GHz, 71 at ≤ 4 GHz, 62 above. Values above it are clamped with an `omnicore:GainLimited` warning; the same applies when a frequency change puts the existing gain over the limit |
| `RxGainMode` | `'manual'`, `'fast'`, `'slow'`, `'hybrid'`; `RxGain` only takes effect in manual |
| `TxGain` | dB, 1×2, 0 – 89.75 in 0.25 steps, equal to 89.75 − attenuation (consistent with the SoapySDR plugin and Pluto). The read-only `TxAttenuation` gives the corresponding attenuation |
| `RxChannels`, `TxChannels` | `1`, `2`, `[1 2]`, or `[]` (turn that direction off). Cannot be changed while streams exist |
| `IQBits` | 12 (default) or 8. With 8-bit, both RX and TX must be `[1 2]`; see [8-bit USB IQ](iq8.md) |

Out-of-range values raise `omnicore:InvalidArgument`, and the configuration stays unchanged. To change several parameters at once, use `configure(dev, Name, Value, ...)`: it sends only once, and when changing sample rate and bandwidth the stream pauses only once.

**Status and board settings**: `info(dev)` (serial number, firmware, USB speed, channel count), `status(dev)` (temperature, reference / PPS / VCXO lock, whether time is valid, RSSI). Properties `ClockSource`, `TimeSource`, `RxLOSource`, `TxLOSource`, `PpsEnable`, `RssiEnable`, `RgbEnable`, `FanEnable`, `RxDCCorrection`, `RxIQCorrection`.

## Receive

### One-shot capture

```matlab
[x, meta] = receive(dev, N);
[x, meta] = receive(dev, N, 'OutputDataType', 'int16', 'StartTime', t0 + 0.5);
```

`x` is an N × `numel(dev.RxChannels)` complex array, one column per channel:

| `OutputDataType` | Type and range | Per sample per channel |
|---|---|---|
| `'single'` (default) | complex single, ±1 | 8 bytes |
| `'double'` | complex double, ±1 | 16 bytes |
| `'int16'` | complex int16, 12-bit samples left-aligned (low 4 bits are 0) | 4 bytes |
| `'int8'` | complex int8, only available with `IQBits = 8` | 2 bytes |

Estimate memory from this: 1e8 dual-channel samples is 1.6 GB as single and 0.8 GB as int16. Other options: `'Timeout'` (seconds to wait for each block of data, default 1) and `'BufferBytes'` (host ring buffer, default 64 MiB).

`meta` fields:

| Field | Meaning |
|---|---|
| `Time` | Hardware time of the first sample, in seconds (NaN when unknown) |
| `TimeTicks` | Same as above, as a `uint64` sample count without loss of precision |
| `Overflow` | Whether samples were dropped |
| `Gaps` | K × 2: `[row of the first sample after the drop, number of samples lost]`; NaN when the count is unknown |
| `Late` | For a timed start, the start time had already passed |
| `Reconfigured` | The device parameters were changed during the capture |

Lost samples are not zero-filled: `x` contains only the samples actually received, so when samples are dropped `receive` returns fewer than N rows. The number lost is computed from the hardware time in each block's header and is exact.

### Continuous receive

```matlab
rx = rxStream(dev, 'OutputDataType', 'single', 'BufferBytes', 256*2^20);
start(rx);                                 % or start(rx, 'StartTime', t)
for k = 1:1000
    [x, meta] = read(rx, 65536);           % 65536 × channels each time
end
stop(rx);
```

Between two `read` calls, the device keeps writing data into the host ring buffer; samples are only dropped when MATLAB cannot keep up and the buffer fills, and samples dropped between two `read` calls are recorded in row 1 of the next `meta.Gaps`. The buffer stores the wire format (3 bytes per complex sample for 12-bit, 2 bytes for 8-bit): 64 MiB is about 0.36 s at dual-channel 30.72 MSPS. At high rates with slow processing, consider capturing everything at once with `receive` and processing afterwards, or increase `BufferBytes`.

When `read` cannot get N samples in time, it returns what it has received; if none arrived at all, it raises `omnicore:TIMEOUT` (the wait time is set by `rx.Timeout`).

Each device has only one RX stream: while an `RxStream` is running, `receive` and a second stream raise `omnicore:BUSY`.

## Transmit

TX input is an N × `numel(dev.TxChannels)` array: single / double complex values are scaled to ±1 (anything beyond is clipped with an `omnicore:TxClipped` warning; real values are treated as Q = 0), int16 is 12-bit left-aligned, and int8 is only available in 8-bit mode. With a single channel, a row vector also works.

```matlab
dev = omnicore.Device('TxChannels', 1, 'TxFrequency', 2.45e9, 'TxGain', 40);
x = single(0.5 * exp(1j*2*pi*1e6/dev.SampleRate*(0:30719).'));

s = transmit(dev, x);                          % one burst; returns after the device reports the burst ended
s = transmit(dev, x, 'StartTime', time(dev) + 0.5);   % timed burst

transmitRepeat(dev, x);                        % loop in the background, returns immediately
...                                            % MATLAB can do other work
s = stopTransmit(dev);

tx = txStream(dev);                            % write in segments
write(tx, x1, 'StartTime', t);                 % a write with a time starts a new burst
write(tx, x2);
write(tx, x3, 'EndBurst', true);
s = status(tx);
stop(tx);
```

Event statistics returned by `transmit`, `stopTransmit`, and `status`:

| Field | Meaning |
|---|---|
| `Underflow` | The device ran out of samples while transmitting (RF underflow) |
| `Starved` | The host queue ran empty during a burst. This also appears while a timed burst is waiting for its start time, where it is harmless |
| `Late` | The start time of a timed burst had already passed (`transmit` additionally gives an `omnicore:TxLate` warning) |
| `BurstAck`, `AckTime`, `AckTicks` | Number of burst ends acknowledged by the device, and the hardware time of each acknowledgement |
| `Errors`, `LastError`, `Dropped` | TX errors reported by the device, the last error message, and the number of events dropped when the event queue was full |

The device only starts transmitting once the queue holds at least one USB transfer (65472 samples for single channel, 32736 for dual channel) or a burst has ended. When writing in segments, data must be fed within a burst at no less than the sample rate, otherwise it underflows. Changing the TX LO makes the core pause TX and discard queued samples; changing TX gain does not.

Each device has only one TX stream: while `transmitRepeat` or a `TxStream` is active, `transmit` raises `omnicore:BUSY`.

### Starting and stopping streams in full duplex

RX and TX can run at the same time, but with one core restriction: once either stream has stopped, a new stream can only be opened when **both directions are stopped** (the core resets the USB pipes when both sides are stopped). So while TX is transmitting, RX can be started once, but cannot be stopped and started again, and vice versa; violating this raises `omnicore:BUSY` ("stop both directions before reopening a stream"). In practice:

- For full-duplex measurements, use one `RxStream` (or `Receiver`) that keeps running paired with a `TxStream` (or `Transmitter`), rather than calling `receive` for each measurement. `examples/timed_burst_loopback.m` does exactly this, finding each timed burst by timestamp in the running RX stream.
- When you need to switch to a different approach, stop both directions first (`stop`, `stopTransmit`, `release`).

## System objects and Simulink

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

- `Receiver`: `[y, valid, overflow] = rx()`. Properties `SamplesPerFrame`, `OutputDataType`, `ChannelMapping`, `SampleRate`, `BufferBytes`, `Timeout`; tunable properties `CenterFrequency`, `Gain`, `GainMode`. When a frame is not completely received, `valid` is false and the missing part is zero-filled.
- `Transmitter`: `underflow = tx(x)`. Properties `ChannelMapping`, `SampleRate`, `Latency`, `BufferBytes`, `Timeout`; tunable properties `CenterFrequency`, `Gain`. Consecutive frames form one unbroken burst.
- A numeric property of `NaN` (`''` for `GainMode`) keeps the device's current value. Changes to tunable properties take effect on the **next call** without interrupting reception.
- Without `Device`, the object opens the device itself by `SerialNumber` (objects with the same serial number share one device) and closes it on `release`; a `Device` passed in is never closed.
- **Full duplex**: channels cannot change while streams exist, so either pass the same `Device` (opened with `RxChannels` and `TxChannels` set) to both objects, or call `setup(rx); setup(tx, x)` before you start calling them. Simulink initializes all blocks before the simulation starts, so two blocks with the same `SerialNumber` work in full duplex directly.
- **`Latency`**: on its first call, `Transmitter` first sends this much zeros (0.1 s by default), so the device runs behind the writes. When transmitting frame for frame at the same rate as RX, this is the margin for how long MATLAB can stall without underflowing.

**Simulink**: add a *MATLAB System* block, set the System object name to `omnicore.Receiver` or `omnicore.Transmitter`, and run it in "Interpreted execution" mode (code generation is not supported). With `SampleRate` set, the `Receiver` sample time is `SamplesPerFrame / SampleRate`; simulation speed is set by the `Receiver` waiting for each frame, i.e. it runs in real time. `examples/simulink_duplex_example.m` builds and runs a full-duplex model from a script; `save_system('omnicore_duplex')` saves it.

Known behavior:

- The first time a Simulink model runs after starting MATLAB, compilation causes a pause of more than 0.2 s, and `Transmitter` may report one underflow; later runs do not.
- R2024b gives "not recommended" warnings for two mixins (`Propagates`, `SampleTime`); they are kept to support R2018a.
- Simulink does not allow character parameters to be tunable, so in Simulink `GainMode` can only be set before simulation.

## Hardware time and PPS

Hardware time is a 64-bit sample count (1 tick = 1 sample); see [Hardware Timestamps](timestamps.md).

```matlab
t = time(dev);              % seconds; timeTicks(dev) is the uint64 sample count
dev.TimeSource = 'external';   % 'none', 'external' (PPS input), 'internal' (on-board PPS), 'sync'
setTime(dev, 0, 'AtNextPPS', true);     % sample time is 0 at the next PPS edge
[tp, ticks] = lastPPS(dev);             % time of the most recent PPS edge
dev.ClockSource = 'external';           % external 10 MHz reference, see [Reference Clock](reference-lo.md)
```

- After `setTime`, the `'StartTime'` of `receive`, `transmit`, `write`, and `start` are all measured on this time; a whole-second `StartTime` falls on a PPS edge.
- PPS and the sample clock are not synchronized, so the time latched on a PPS edge has ±1 sample jitter: after alignment, `lastPPS` may read the whole second minus 1.
- Changing the sample rate restarts the hardware time. If the time had been set, an `omnicore:TimeInvalidated` warning is given. The "valid" state of the hardware time persists after closing and reopening the device.

**Events**: `e = events(dev)` retrieves device events (the most recent 256 are kept), with fields `Type`, `Message`, `HwTime`, `HwTicks`, `Count`, `HostTime`. USB transfer errors are also merged into one `omnicore:Transport` warning when the next call returns.

## Errors, interruption, and unloading

- Error identifiers are always `omnicore:<reason>`: `BUSY` (device or stream in use), `NOT_FOUND`, `TIMEOUT`, `InvalidArgument`, `InvalidHandle` (the device behind the object has been closed), `IO`, and so on; the message is OmniCore Host's error description. Handle them with `try ... catch err, switch err.identifier`.
- **Ctrl+C**: MATLAB only responds to Ctrl+C between MEX calls, so a very long `receive` has to return first. After an interruption the objects are intact; just `stop(rx)` or `delete(dev)`. When built with the CMake option `-DOCH_MATLAB_CTRL_C=ON` (or `build_omnicore_mex(..., 'CtrlC', true)`), long calls stop within 0.1 s and raise `omnicore:Interrupted`. This option uses an undocumented MATLAB function, so it is off by default.
- While a device is open, the MEX file is locked and `clear mex` does not unload it; `clear all` first deletes the objects and closes the device, then unloads. Exiting MATLAB closes all devices automatically.
- Objects close their device or stream on `delete`; failures only give a warning, not an error.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `omnicore:BUSY`, `device already owned by another OmniCore Host process` | Another program (`och`, GNU Radio, another MATLAB) has the device open |
| `omnicore:NOT_FOUND` | No device connected, or the driver / udev rule is not installed; check with `och find` first |
| Windows: `omnicore:LibraryNotFound` | `omnicore.dll` / `libusbK.dll` missing next to the MEX file, or not on PATH |
| macOS: the MEX file "cannot be opened because the developer cannot be verified" | Run `xattr -dr com.apple.quarantine omnicore-matlab` |
| Linux: `GLIBCXX_3.4.xx not found` | The MEX file or `libomnicore` was built with a newer GCC, but MATLAB loaded its own older libstdc++ first. On Linux the MEX file always links the C++ runtime statically; `libomnicore` needs `-DOCH_STATIC_LIBSTDCXX=ON` (the release package has it). You can also start MATLAB temporarily with `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6 matlab` |
| Frequent `meta.Overflow` during continuous receive | MATLAB is processing too slowly: increase `BufferBytes`, do less processing per frame, or capture everything at once with `receive` |
| Occasional `Transmitter` underflow | Increase `Latency`; avoid plotting or printing inside the loop |
| Changing `RxChannels` raises `omnicore:BUSY` | `stop` or `release` all streams first |
