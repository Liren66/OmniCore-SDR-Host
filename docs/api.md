# C/C++ Development Interface

## Linking and ABI

The public C header is omnicore/och.h. On Windows, link the import library for your toolchain that corresponds to omnicore.dll; on Linux link libomnicore.so.1, and on macOS libomnicore.1.dylib (on both, the `och` program finds the library through a relative RPATH). omnicore/och.hpp is a C++17 RAII wrapper compiled by the caller. The release SDK provides both a native MSVC omnicore.lib generated from the DLL export table and MinGW's libomnicore.dll.a; they cannot be swapped by renaming the extension.

Adapter projects can pick the right import library through the installer's CMake configuration:

~~~cmake
cmake_minimum_required(VERSION 3.20)
project(my_receiver LANGUAGES CXX)
find_package(OmniCoreHost 0.8 CONFIG REQUIRED)
add_executable(my_receiver receive.cpp)
target_compile_features(my_receiver PRIVATE cxx_std_17)
target_link_libraries(my_receiver PRIVATE OmniCore::omnicore)
~~~

When configuring, point CMAKE_PREFIX_PATH at the OmniCore Host install directory. At runtime on Windows, put the install directory's bin on PATH, or place a DLL of the matching version in the application directory. The release pipeline builds the MATLAB MEX with the MSVC import library from the installer; this is the MSVC usage that has actually been verified so far.

Output structs with struct_size must first be zeroed and set to sizeof(struct); for configuration/stream arguments, prefer initializing with och_default_config and och_default_stream_args. The ABI version is OCH_ABI_VERSION, and the software version comes from och_version(). Plugins must not depend directly on internal structures.

Errors are returned as och_result. och_last_error() gives the failure details for the calling thread and should be copied immediately, since the next failure may overwrite it. C++ control methods throw exceptions through och::check, while recv/send keep the TIMEOUT/STOPPED return values for loops to handle.

## Lifecycle

1. list enumerates, and open opens by serial number. Without a serial number, it only selects the single device.
2. configure sets the shared sample rate, masks, LO, bandwidth, and gain, and never transmits implicitly.
3. create_stream creates a single-use stream for a direction and allocates its buffers.
4. RX: after start, recv or acquire/release. TX: prefill with send, then start, then keep calling send.
5. After the last TX send you can flush; no send may run concurrently during flush.
6. stop, wait for the application's I/O threads to exit, release leases, then destroy_stream.
7. close after all streams are destroyed; close returns BUSY while streams still exist.

After an explicit stop, a stream cannot be started again; destroy and recreate it. The current CH32 buffer recovery constraint requires that, after one direction stops, the other direction is stopped before reopening; if the other direction is still active, BUSY is returned. Since 0.3, parameter changes during streaming are coordinated by the device, so the application does not need to stop/destroy/create for every change; an internal pause does not put a stream that was running into a one-shot terminated state. See the next section.

The device must outlive its streams. The examples declare the device before the streams and rely on reverse-order destruction. Destructor cleanup is not used to report critical errors; for a normal stop, call stop/close explicitly and handle the return values.

destroy_stream returning BUSY means there is still a lease and the handle is kept; OK or IO both mean the handle has been destroyed, so even if an IO error occurred while stopping the hardware, it must not be destroyed again. The C++ close already handles this ownership rule. A BUSY from device close keeps the device object; retry after its streams are destroyed.

get_config returns the most recently successfully applied configuration request for that handle, and is only available once that handle has been configured. It is not a live read-back of all RF registers; for example, hardware quantization of LO/clocks and actual AGC gain changes are not reflected automatically in this cache. To check the actual state, query the corresponding SDK command; requested values must not be treated as measurements.

open takes ownership of the device, confirms the protocol version, stops any old USB data session, and uses the existing CH32 diagnostic interface to clean up cross-session data. It is not a completely side-effect-free observation interface. The standalone CLI's info/get/config also need open; if another OCH process already holds the device, the second process is refused. list/find only enumerate and do not take over the data streams.

## Changing parameters at runtime

In the **same process** that holds the device, a management thread can call och_configure / device.configure directly, or the typed FIR, clock, sample-rate, RX correction, reference, and LO source setters. The core coordinates pausing RX/TX, waits for current I/O operations to leave their critical sections, applies the parameters, cleans up cross-session data, and then resumes the directions that were running before the change. The original och_stream handles, formats, USB request buffers, and large host buffers stay stable, and callers keep using the same stream objects.

- While any stream object exists, neither rx_mask nor tx_mask can change; changing the channel layout requires destroying all streams first. Stream format and buffer sizes are still fixed at create_stream.
- When the effective configuration of configure is exactly the same as the request already applied, it returns immediately without interrupting streaming. When something changes, only the changed RF configuration is written, avoiding unrelated clock/bandwidth writes and calibrations; this does not promise that every parameter can be changed seamlessly.
- Only directions that were running are resumed. Streams not yet started stay unstarted, and streams explicitly stopped stay stopped. Receive-only applications never start transmitting because of this.
- A recv, acquire, or send that is waiting may temporarily return OCH_TIMEOUT; consumers/producers keep the same handle and retry. During reconfiguration, TIMEOUT may return immediately, so sleep briefly before retrying (e.g. 1 ms) to avoid a busy loop that burns CPU and repeatedly contends for the I/O lock. When send has accepted part of the samples, it still returns OCH_OK; advance by accepted and never resubmit the accepted prefix.
- While an RX lease has not been released, OCH_BUSY is returned and the hardware is unchanged. Applications using leases should first coordinate with the consumer to release it, then retry the change; you cannot hold the pointer while waiting for reconfiguration to finish.
- Control and lifecycle calls are executed serially by one management thread. destroy/close must not run concurrently with any operation on that device/stream; flush should not run concurrently with reconfiguration or send either.

During RX reconfiguration the old queue is discarded, and known local host drops are counted in host_dropped_samples. The first framed block after resuming starts a new epoch (monotonically increasing), carrying OCH_META_START | OCH_META_DISCONTINUITY | OCH_META_RECONFIGURED (value 32) | OCH_META_HAS_TIME. The reconfiguration itself is never disguised as DEVICE_OVERFLOW; there is no sampling during the pause, and `time_ticks` gives the position of the new segment on the timeline, but changing the sample rate invalidates the hardware time (`OCH_EVENT_TIME_INVALIDATED`). Downstream code should clear filter, sync, and demodulation history at this boundary and interpret subsequent data with the new configuration.

During TX reconfiguration, old queued data is discarded and old USB requests are cancelled, so samples from the old configuration are never replayed under the new one. A direction that was transmitting is automatically prefilled with 4 zero-IQ blocks of transfer_bytes each before resuming, with the samples counted in tx_padding_samples; after the new configuration and the zero blocks are submitted, the original producer is first allowed to continue sending, refilling the queue while the hardware starts, then RX resumes first and TX starts last. Producers should keep retrying after TIMEOUT and need not wait for the management thread's configuration call to return; if resuming fails, the core blocks TX data operations again and stops both directions. A TX that was not started or was already stopped is never started by this. Samples accepted by earlier sends are not guaranteed to have left the DAC, and may also be discarded during reconfiguration.

### Fast tuning

If, relative to the last successful configuration, only some of rx_frequency, tx_frequency, rx_gain, and tx_attenuation changed, configure **sends the commands directly without pausing the streams**: queues are not flushed, USB transfers are not reset, no TX zero blocks are prefilled, and the epoch is unchanged. Afterwards, the next RX block carries OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY. This boundary is approximate: samples still in the USB queue when the command returns (about usb_queue_depth × transfer_bytes) may already be at the new frequency, or may be data from while the PLL was settling. Applications that need an exact boundary should discard a short stretch of samples around the flag.

- Changing tx_frequency while a TX stream exists still goes through the pause transaction above, because a TX LO jump of more than 100 MHz triggers TX quad calibration, which should not happen while transmitting.
- Manual RX gain is only rewritten when the LO crosses an SDK gain-table band (1.3 GHz / 4 GHz, treated as crossing within ±1 MHz of the boundary) or when the gain itself changes.
- If fast tuning fails midway, it is handled as a reconfiguration failure: both directions stop and the configuration cache is invalidated.
- On success, OCH_EVENT_RECONFIGURED is emitted with the message `retune complete streams_continued=1 ...`.
- The capability table (system_info?) is read and cached at open; the external LO selection is also cached until set_lo_source or a raw modifying command invalidates it.

Measured (USB3, RX stream running, control UART at 3.125 MBd): about 4 ms for a retune within the same band, about 25 ms across gain tables (mostly SPI writes of the gain table), with no RX interruption. The time is mainly spent on control UART round trips and the AD9361's VCO calibration and lock wait; for the round-trip times of each command see [Device Protocol: Control UART](protocol.md#control-uart), and for a breakdown of continuous sweeping see the [sweep example](sweep.md#measured-results-2026-11-14). On older firmware where the control UART is still at 115200 baud, a same-band retune takes about 11 ms.

OCH_EVENT_RECONFIGURED (new value 6) reports the resumed directions of a successful reconfiguration, discontinuities/drops, and the duration measured by the host; failures are reported through OCH_EVENT_TRANSPORT. Events are observation records of control operations, and IQ boundaries are determined by the RX metadata. Hardware reconfiguration and recovery tests have been completed for single-channel RX, zero-IQ TX, and bidirectional modes. The durations are host API and observation times, not hardware sampling gaps, and there is no fixed millisecond latency guarantee.

When a hardware change or resume fails, both streams stop and the device configuration cache is invalidated; nothing is rolled back and transmission does not restart automatically. The application should keep the error details, stop and wait for the I/O threads to exit, destroy the old streams, and after confirming the device state, explicitly configure and recreate/start the streams; if the SDK reports rollback=failed / applied=unknown, first complete rf.reinit as described for FIR. Argument validation failures and BUSY caused by leases are not failures of an executed hardware transaction.

This capability does not provide a cross-process control service: a separate och process cannot modify a device held by software that is receiving. Raw och_command modifications are still forbidden while stream objects exist, so raw commands cannot bypass the coordinating interface. See the [continuous receive with reconfiguration example](../examples/reconfigure.cpp). The ABI is still 1 and existing public struct layouts are unchanged; the new semantics need a 0.3 or newer DLL, and the new RX correction functions need a DLL that includes those exports.

## RX DC and IQ correction

0.3.1 added a standalone och_rx_correction_config, without extending och_config or och_device_info. Zero the output first and fill in struct_size; dc_enabled and iq_enabled are both 0/1 and indicate whether the AD9361's native tracking correction is on.

| C function | C++ device wrapper | Purpose |
|---|---|---|
| och_get_rx_correction | rx_correction() | Read back the current DC and IQ state |
| och_set_rx_dc_correction | set_rx_dc_correction(bool) | Control RF and baseband DC offset tracking together |
| och_set_rx_iq_correction | set_rx_iq_correction(bool) | Control RX quadrature tracking |

Both settings apply to both physical receive channels and do not become per-channel switches based on the rx_mask selection. They are the chip's receive tracking functions; they do no host IQ processing and do not trigger TX IQ calibration. 0 stops continuous tracking updates and does not clear correction coefficients already obtained; IQ=0 still keeps the hardware phase/gain correction path, so it is not a complete bypass of IQ correction. 1 enables tracking and does not mean residual DC or image rejection has been measured.

~~~cpp
auto correction = dev.rx_correction();
dev.set_rx_dc_correction(true);
dev.set_rx_iq_correction(true);
~~~

Each call first checks rx_correction=1 in system_info?; if it is missing or not 1, OCH_UNSUPPORTED is returned and existing streams keep running. Queries do not pause streams. The setters reject values other than 0/1; a setting equal to the device read-back succeeds immediately without flushing queues or creating a new epoch. When a change is really needed, both directions are paused using the coordinated flow described earlier in this section; afterwards both states are read back to confirm the target value took effect and the other one did not change, and then the directions that were running are resumed. A write failure or inconsistent read-back stops both directions and invalidates the configuration cache, and the application recovers following the reconfiguration failure flow.

A normal configure does not write these two switches; the SDK's RF rebuild and FIR/clock operations restore the most recently saved tracking targets, and so does rf_reinit. Both default to 1 on cold start, and a request to turn them off is kept in RAM only, so it does not survive a power cycle/reloading the program. get_config does not include them; query them with the dedicated getter. If the SDK detects a mixed RFDC/BBDC state or an SPI failure, it reports an error rather than disguising the mixed state as 0; if the error reports rollback=failed / applied=unknown, run rf_reinit first, then reconfigure and recreate the streams. Older SDKs without this capability can still use the features they supported; do not use the firmware name in place of the capability field. The new API requires omnicore.dll and headers from 0.3.1 or later.

## USB-C boot power snapshot

0.2.1 added och_get_power_status(device, &status, timeout_ms), and in C++ device.power_status(timeout_ms=500). Zero och_power_status and fill in struct_size first. Since 0.7.1, with bridge firmware 1.0.1 or later (which has EP 0x03 OUT), the function first drains EP83, then sends a `typec?` query and parses the `HD3SS3220:` reply that follows, with live=1; with bridge firmware 1.0.0, it reads the HD3SS3220 boot register snapshot from the EP83 debug log, with live=0.

timeout_ms=0 only returns the cache within the current device handle; 1..2000 allows a bounded read of EP83 when no stream objects exist. While stream objects exist, only the cache is read, so it never competes with high-speed streams for the log endpoint. The cache is discarded when the device is closed and is never written to disk. A successful read consumes the data of this USB debug endpoint; if another program read the log earlier, there may be no snapshot available.

A missing log, no EP83, or a wait timeout all return OCH_OK normally, with snapshot_valid=0 and supports_3a=-1. Returning OK only means the query completed, not that the power state is known. raw_08/raw_09/raw_0a, attached_state, and current_detected can only be interpreted when snapshot_valid=1 and conflicting_snapshots=0. Only a valid UFP connection can have supports_3a of 0/1; otherwise it is -1. source_current_ma=-1 means unknown, including the USB default current advertisement, and must not be filled in as 500/900 mA on your own.

The log has no reliable boot number or sample timestamp, and conflicting snapshots stay unknown. It is not a live current/voltage measurement, a USB-PD contract, an LED GPIO read-back, or a test of the supply's capability under load. The CLI converts unknown values to JSON null and always labels them source=startup_log, live=false; applications should keep these limitations too, and must not just treat a non-zero supports_3a as true, because -1 is itself non-zero.

Calling this new API requires a 0.2.1 or newer DLL. The existing och_device_info layout is unchanged; och info --json adds a power object alongside the existing fields, and och find still only enumerates devices.

## FIR and shared clocks

0.2 added och_fir_config, och_clock_config, and the corresponding functions, keeping existing ABI 1 struct layouts unchanged. Calling the new functions requires a 0.2 or later DLL that includes these exports; upgrading the header does not give an old DLL the new capabilities.

| C function | C++ device wrapper | Purpose |
|---|---|---|
| och_get_fir | fir(direction) | Snapshot of the FIR configuration most recently verified over SPI by the SDK |
| och_set_fir | set_fir(config) | Upload in a chunked transaction, verify, and set one direction |
| och_set_fir_enabled | set_fir_enabled(rx,tx) | Set enable for both sides in one transaction |
| och_get_clock | clock() | Read back the actual reference clock and both sets of six-stage clocks |
| och_set_clock | set_clock(config) | Check and set the complete RX/TX chains |
| och_set_sample_rate | set_sample_rate(hz) | The SDK safely computes and applies the shared sample rate; when no exact clock solution exists, the nearest usable rate is used instead |

For the FIR configuration, zero it first, fill in struct_size, direction, channel_mask=3, factor, tap_count, gain_db, and coefficients, then choose enabled. The CLI's load defaults to enabled=0; the C/C++ API can set the enable state explicitly. verified and crc32 are read results; when setting, the core computes and verifies the CRC. The tap count is a multiple of 16 and at most 128, with signed16 coefficients; RX gain is -12/-6/0/6 dB, TX gain is -6/0 dB, and factor is 1/2/4; TX factor 1 allows at most 64 taps, and the hardware clock budget may lower the limit further. Both channels of the same direction use the same set of coefficients.

In clock, rx_hz is BBPLL/ADC/R2/R1/RF/Fs and tx_hz is BBPLL/DAC/T2/T1/TF/Fs, all in Hz. reference_hz reads the fixed 40 MHz board reference clock, and writing 0 means keep it. Since 0.3.2, `och_set_reference_source` can enable disciplining to an external 10 MHz through the ADF4002, but the AD9361 input is still 40 MHz and cannot be changed through `och_set_clock`. The BBPLL and Fs of RX/TX must match. Valid dividers and FIR constraints are verified by the SDK, and implicitly lowering the sample rate to satisfy the limits is not accepted.

**Nearest usable sample rate**: the BBPLL is a fractional divider (40 MHz reference, modulus 2088960); the driver programs the nearest fractional word and rounds down on read-back, and the SDK requires the read-back to match the request to the Hz, so some rates are rejected; for example, 11.2 MSPS needs a BBPLL of 716.8 MHz. `och_set_sample_rate` and `och_configure` reproduce on the host the clock chain selection of the AD9361 driver's `ad9361_calculate_rf_clock_chain` (using the current FIR decimation / interpolation factors) and the BBPLL rounding, search from the requested value at +1, −1, +2, −2 Hz, and so on for the first whole-Hz rate that can be hit exactly, and submit it to the device only once. 11.2 MSPS becomes 11200011 Hz; the offset is usually within 20 Hz (at most about 33 Hz when modeled over the whole range). If the device still rejects it (firmware and model disagree), it then tries Hz by Hz on the device, up to ±64 Hz. The actual rate is written to `sample_rate` in `och_get_config`, and a later request for the original rate on the same handle reuses it directly. Rates outside the clock or FIR range (such as 1 MSPS without FIR decimation enabled) still fail with the original error. A complete clock chain specified with `och_set_clock` is never substituted.

Since 0.3, typed FIR/clock writes use the coordinated reconfiguration flow described above and can be called while streams exist; all management operations should still run serially. After a hardware write failure, both directions stop; destroy the old streams, and after any necessary rf.reinit succeeds, configure again and create the streams; do not treat the cache from before the failure as the known actual state. A successful FIR change keeps the current Fs, and a normal configure keeps the loaded FIR. After changing Fs, recheck the frequency response. Modifications through the raw och_command still require no stream objects and invalidate the configuration cache.

Capability checks for FIR/clock/sample-rate and the rejection of the fixed reference clock happen before streams are paused: when an older SDK does not support the extension, or a reference_hz request does not match the fixed board reference clock, existing streams are not stopped and the IQ queues are not flushed. Failures after the actual write has started are still handled as reconfiguration failures.

FTR parsing belongs to the CLI; the public C API accepts an already validated fixed-length coefficient array and does not depend on a file format. verified/CRC32 prove configuration consistency, not the filter's frequency response or RF performance. For the detailed file format and lifecycle, see [FIR and Clocks](fir.md).

## ADF4002 reference and external LO

For the reference and LO APIs, the PPS frequency LOCK (0.3.4), declaring external frequencies, and the semantics of restoring the internal frequency, see [ADF4002 and External LO](reference-lo.md). `och_config` contains no source selection fields; `configure` keeps the independent selection, and a successful LO source setter syncs the actual nominal frequency in the configuration. The default sources are determined by the SDK start-up configuration, internal for both RX and TX.

## Samples and formats

The capacity/return count of recv and the input/accepted count of send are all **complex samples per channel**, not scalar counts or totals across channels. Dual channel is time-major: I1(t0),Q1(t0),I2(t0),Q2(t0),I1(t1),Q1(t1)... When only channel 2 is selected, only that physical channel's I/Q is present.

| Format | Per complex sample per channel | Definition |
|---|---:|---|
| packed12 | 3 B | I=(b0<<4)\|(b1>>4), Q=((b1&15)<<8)\|b2; each sign-extended from 12 bits |
| sc16 | 4 B | Little-endian int16 I/Q; the signed 12-bit wire value left-aligned by 4 bits |
| fc32 | 8 B | Little-endian float I/Q; the wire value divided by 2048 |
| sc8 | 2 B | Signed int8 I/Q; only available with usb_iq_bits=8 |

With a dual-channel recv capacity of 65536, an sc16 buffer needs at least 65536×2×2×sizeof(int16_t) bytes. The 16-bit format is not the unscaled -2048..2047.

TX sc16 drops the low 4 bits; fc32 is quantized to 12 bits with round half away from zero, out-of-range values saturate to -2048..2047, and NaN maps to zero. Adapters must not rely on out-of-range input keeping its value. Internally, a vectorized implementation supported by the CPU may be chosen, with the public format and numeric semantics kept the same.

`och_config.usb_iq_bits` uses the former first reserved slot; the struct is still 104 bytes and ABI 1 is unchanged. 0/12 use the default 12-bit wire described above; 8 is only allowed with `rx_mask=tx_mask=3` and needs SDK `iq_format=1 iq8_2t2r=1`. The wire format cannot change while any stream object exists; destroy the streams first. 8-bit RX restores signed8 as sc16×256, fc32÷128, or packed12 with the low 4 bits zero; TX applies the original 12-bit quantization and then drops the low 4 bits. Application format sizes do not change with the wire; SC8 passes through. See [8-bit extension](iq8.md).

## RX metadata and leases

A single recv/acquire never mixes several epochs or known discontinuities:

- epoch: incremented at stream start, device GAP, late command, reconfiguration, or any time discontinuity; it is not the hardware block sequence number.
- sample_index: position per channel within the current epoch.
- time_ticks / tick_rate: hardware time of the first sample in this batch (valid when flags contain HAS_TIME) and the corresponding sample rate, 1 tick = 1 sample.
- flags: START, DISCONTINUITY, HOST_OVERFLOW, DEVICE_OVERFLOW, RECONFIGURED, HAS_TIME, END_OF_BURST, LATE. TIME_UNKNOWN is no longer produced.
- host_dropped_samples: known local host drops.
- device_dropped_samples: number of samples dropped by device overflow, an exact value derived from the block header time difference.
- host_monotonic_ns: host observation time, not the ADC sampling time; use time_ticks for the sampling time.

Data blocks and EVTs arrive on different USB endpoints, so the arrival time of an EVT cannot replace the data boundary. Consumers should reset sync, filter history, or frame parsing based on the IQ metadata.

och_rx_acquire returns a lease in the current wire format, with capacity in bytes, independent of args.format. 12-bit is 3 bytes per complex sample and 8-bit is 2 bytes; dual channel is 6/4 bytes per time frame respectively. Each stream can have at most one lease, and before releasing it you must not acquire, recv, or destroy again; reconfiguration during streaming returns BUSY while a lease exists. The pointer becomes invalid immediately after release. Keep processing time bounded to avoid holding the core queue for a long time.

## Partial completion and time

send may accept only part of the samples; once any samples are accepted it returns OCH_OK, and the caller always consumes the corresponding prefix by accepted and retries the rest, never resending the whole block. TIMEOUT means no samples were accepted within the deadline, or a temporary interruption due to coordinated reconfiguration; it does not mean a permanent disconnect.

send before start only prefills and does not transmit. After a stop, submitting data later never turns transmission on automatically. flush pads with zeros for alignment and waits for the host queue and OUT transfers to complete; the number of padded samples is in tx_padding_samples. send/flush on continuous streams carry no time; to transmit at a given sample or learn when playback completed, use `och_send_burst` (HAS_TIME/SOB/EOB), whose burst completion produces an `OCH_EVENT_TX_BURST_ACK` with the sample time. For hardware time, PPS time alignment, and timed RX, see [Hardware Timestamps](timestamps.md); phase-coherent LO sync across devices is not provided. The separate PPS interfaces are `och_set_pps_enabled`, `och_get_pps_status`, and `och_get_vcxo_status` (with the internal reference, PPS disciplines the 40 MHz through the CH32 PA4); see [PPS API](pps.md).

After flushing finite data and before stop shuts off the hardware, draining the FIFO may produce `tx_underflow` and `TX_STARVED`. These trailing events are kept as is and should be considered separately from starvation during continuous supply; for why, see [Architecture: Explicit TX start and stop](architecture.md#explicit-tx-start-and-stop). When you need an exact burst end point, use `och_send_burst` and rely on the `hw_time` of `OCH_EVENT_TX_BURST_ACK`.

## Events and threads

Each device has a bounded event queue. och_poll_event retrieves device EVTs, host overflows, transport faults, TX starvation, reconfiguration results, and the timed events of 0.6.0 (`RX_GAP`, `RX_LATE`, `TX_BURST_ACK`, `TX_LATE`, `TX_UNDERFLOW`, `TX_ERROR`, `TIME_INVALIDATED`; `hw_time`, `code`, and `block` give the sample time, FPGA event code, and block sequence number; since 0.6.1, `count` is the number of device events the record stands for, greater than 1 when the firmware merges underflow/GAP events during overload; see [Hardware Timestamps](timestamps.md)). Applications should drain it regularly; an increasing event_drops means the queue was not processed in time or events were too dense.

RX and TX can run in parallel; each RX has only one consumer and each TX only one producer. stop and coordinated reconfiguration can run in parallel with ongoing RX/TX, to end or temporarily interrupt waits; destroy/close must not run concurrently with any operation on that handle. Destroy objects only after confirming the I/O threads have exited. The application manages configuration and lifecycle operations serially.

och_command is the SDK's raw control entry point; modifying commands are only allowed when no stream objects exist, and afterwards you should configure to re-establish a consistent configuration. Plugins should prefer the standard configuration API and must not open the same USB device separately to compete with the core.

## Statistics

- usb_bytes is the number of bytes of completed USB requests for the stream; for RX it includes the 16 B block headers and padding, and for TX it excludes data only accepted into the host queue whose USB transfer has not completed.
- samples is, for RX, the number of per-channel samples delivered to the application, and for TX, the number of per-channel samples accepted into the host queue. Neither is an RF measurement; in particular, TX cannot use it to assert that all samples have been transmitted.
- host_dropped_samples, headers (number of framed blocks parsed), resyncs (number of resynchronizations after a block header check failure, normally 0), transfer_errors, and tx_starvation_events belong to the stream; queue bytes and peaks are counted in current wire bytes, 3 B per complex sample for 12-bit and 2 B for 8-bit.
- device_overflows, device_underflows, and event_drops belong to the observation period of the device object; RX/TX stats see the same device counts, so do not add the two together. device_overflows counts received GAP blocks, with the exact number of dropped samples in device_dropped_samples of the RX metadata; device_underflows counts TX underflow events (merged records accumulate by `count`).
- The maximum sample rate of 61.44 MSPS is a single-channel RF capability and does not mean USB can carry all four channels at full speed at once. Four channels at 61.44 MSPS need 737.28 MB/s on the wire in total; choose the bidirectional rate based on the shared USB bandwidth. Common four-channel tests use 30.72 MSPS.

## RX RSSI

`och_get_rx_rssi(device, &rssi)` returns `och_rx_rssi`, containing `enabled`, `usb_rx_mask`, `valid[2]`, and `rssi_db[2]`; `och_set_rx_rssi_enabled(device, 0|1)` turns it on and off. The C++ interface is `device.rx_rssi()` and `device.set_rx_rssi_enabled(bool)`. RSSI reporting is off by default after an SDK cold start, in which case `enabled=0` and both channels are invalid. The value is the AD9361 symbol RSSI in relative dB, higher is stronger, and is not calibrated input power. When the ENSM is not in the RX/FDD state, `valid=0` and `rssi_db=0`; `usb_rx_mask` only indicates the USB upload mask, and with 2R2T both receivers are measuring. This is a pure query that can be called while RX/TX streams are running without pausing them; measured with dual-channel 30.72 MSPS receive, there were no discontinuities. Newer SDKs (`rssi=1`) use `rx_rssi?`; older SDKs parse it from `system_status?`, which also works but is slower.

## Board RGB, fan, and temperature

`och_get_rgb_status`, `och_set_rgb_enabled`, `och_get_fan_status`, `och_set_fan_enabled`, `och_get_core_temperature` (C++: `rgb_status()`, `fan_status()`, `core_temperature()`, and so on); for capabilities and usage see [Board Control](board-control.md). These calls do not stop IQ streams.

## Bridge firmware

`och_firmware_query`, `och_firmware_read_image`, `och_firmware_update`, `och_firmware_enter_bootloader`, and `och_firmware_start_app` only use EP0 vendor requests and do not open the SDK control channel, so they work in bootloader mode; when the device is held by another handle, they return `OCH_BUSY`. `och_get_firmware_info` returns the bridge firmware information of an open device. While the device is in the bootloader, `och_open` returns `OCH_UNSUPPORTED`. For usage and recovery, see [Bridge Firmware Update](firmware-update.md).

## Hardware time

| Function | Purpose |
|---|---|
| `och_set_time_source(dev, src)` | PPS source: `OCH_TIME_SOURCE_NONE` / `EXTERNAL` (J6) / `INTERNAL` (divided from the AD9361 40 MHz) |
| `och_set_time(dev, ticks, at_next_pps)` | Load a 64-bit sample time immediately or on the next PPS edge |
| `och_get_time(dev, &status)` | `valid`, `armed`, `now`, `last_pps`, `pps_count`, `tick_rate`, `fpga_version` |
| `och_stream_args.start_flags` | `OCH_RX_TIMED` starts at `start_time_lo/hi`; continuous when `num_samples` is 0 |
| `och_rx_command(rx, flags, ticks, n)` | Issue an RX command while running: timed, `OCH_RX_NOW`, or `OCH_RX_STOP` |
| `och_send_burst(tx, samples, n, &md, &accepted, timeout)` | `och_tx_metadata` carries HAS_TIME / SOB / EOB and the sample time |

New fields are all appended at the end of structs or occupy former reserved slots, staying compatible with older callers through `struct_size`; the C ABI is still 1. 0.6.1 appended `count` and `reserved_event` at the end of `och_event`; the `struct_size` of the 0.6.0 and earlier layout is still accepted (without access to `count`). For timing diagrams and examples, see [Hardware Timestamps](timestamps.md).

## Multi-board sync

Requires the firmware capability `sync=1` (SDK_CMD_1.2, FPGA with OC_SYNC_AXI). Single-board interfaces: `och_get_sync_status`, `och_set_sync`, `och_sync_trigger`, `och_sync_clear`, `och_mcs_step`; `och_set_time_source` gains `OCH_TIME_SOURCE_SYNC`; event `OCH_EVENT_SYNC_IN`. Multi-board orchestration: `och_multi_sync(devices, n, &args, results)`, where `devices[0]` is the master and `args.mode` is `OCH_SYNC_CHAIN` (daisy chain, no external reference needed) or `OCH_SYNC_COMMON` (common 10 MHz + PPS). All new structs carry `struct_size` and are initialized with `och_default_sync_config` / `och_default_multi_sync_args`. For the procedure, wiring, and limitations, see [Multi-Board Sync](multi-board-sync.md).
