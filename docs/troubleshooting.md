# Troubleshooting and Limits

## Device not found

First run och-driver check and och find --json, and check whether the target data device 1A86:5537 uses libusbK and enumerated successfully. find does not open the device to query the link speed; unknown in the default text output and usb_speed_mbps=0 in JSON both mean it was not queried. Then run och info to check the actual USB3 / USB2 High-Speed link and whether exclusive open succeeds. Do not switch the WCH-Link to libusbK. With several matching devices you must use --serial.

Device Manager code 43 and port reset failures are USB enumeration problems and must not be handled as stream faults after a successful open. Save the diagnostic information, then check power, cable, and port. If the driver is rejected by security policy, follow the installation notes; signature enforcement and Memory Integrity are never turned off automatically.

## Power query shows unknown

With bridge firmware 1.0.1 or later, och power / och info query the USB-C state live through `typec?` every time (`live=true`), so the situation below does not occur; if it is still unknown, first confirm the bridge version with `och firmware info`. The rest of this section only applies to bridge firmware 1.0.0.

och power status uses the existing CH32 EP83 boot log, and can only report supports_3a=true/false from a complete, consistent, valid UFP snapshot. No EP83, the log not having arrived yet, the log having been consumed by another program, or conflicting snapshots can all produce unknown, which does not mean 3 A is unsupported. info also reads and consumes this log; the cache belongs only to the device handle in the current process and is discarded when the handle is closed or the process exits. So info showing 3 A followed by power status showing unknown does not mean 3 A disappeared or the power supply dropped.

The boot log provides no reliable boot number, live connection state, or LED GPIO read-back. The host only reports the information that exists; it never modifies/downloads the CH32, SDK, or FPGA to obtain a result, and does not pretend to measure the present supply current. If that log shows 3 A, it only describes the detection result in that boot snapshot and does not prove how the cable/supply behaves under the current load.

## Low throughput, RX discontinuities, or TX underflow

On USB2, first check the [USB2 benchmark results](benchmarks.md#usb-20-high-speed) and choose the rate by number of RX/TX channels and IQ precision. The 61.44 MSPS `RF sample-rate ceiling` is not a rate USB2 can sustain. Passing a short test may still underflow after a few minutes, so repeat long tests at the chosen setting; the default 30.72 MSPS examples are meant for USB3.

First run the packed12 benchmark without writing to disk, then test files and format conversion. Dual RX at full speed is 368.64 MB/s on the wire, and saving as fc32 is about 983.04 MB/s; a disk that is fast in short bursts thanks to caching may not sustain that write rate.

USB controllers, shared bandwidth, and scheduling differ between computers. You can try --buffer-mib 128 and watch host_queue_peak_bytes, host_dropped_samples, transfer_errors, and device overflow/underflow, instead of just chasing larger transfers. 64 MiB covers about 182 ms of dual-RX full-speed data.

If a brief RX overflow during bidirectional start is recovered by the device and a new complete header appears, the core keeps receiving and marks a new epoch. Downstream code must reset at the boundary. Persistent repeated overflows mean throughput/consumption is still insufficient; the program not exiting does not make it lossless.

When playback of a finite TX file ends, the tail before the hardware is shut off may report TX underflow and host starvation. This is expected; see [Architecture: Explicit TX start and stop](architecture.md#explicit-tx-start-and-stop). Judge it separately from underflows in the middle of a continuous stream.

## Framed blocks and old data

`och_open` reports `framed USB transport required`: the Zynq firmware is too old and you need Firmware v2026.11.07 (FPGA `CH32H417_IP` 1.3). With older firmware, keep using OCH 0.5.x.

A non-zero `resyncs` (shown as Header errors in the terminal) means block headers with a bad magic number or checksum were received, and the core realigned byte by byte. It should be 0 in normal operation; if it keeps happening, check that the firmware versions match and check the USB cable and port.

Plain IQ files have the block headers removed; a raw USB dump contains a 16 B header every 16 KiB and must first be parsed as described in [Device Protocol](protocol.md#framed-block-format); it cannot be treated as sc16.

A device overflow event (`RX_GAP`) may arrive before or after the corresponding data. Locate it using the epoch/metadata of the data path (the GAP flag and device_dropped_samples); do not cut the current block arbitrarily when the EVT arrives.

## Reopening a direction returns BUSY

The USB cancel/cross-session buffer state of the current CH32 firmware restricts hot-reopening a single direction. Stop and destroy the streams in both directions first, then configure and create. The core will not quietly flush a running RX just to reopen TX. This project has not modified the CH32 to lift this restriction.

## Runtime reconfiguration, TIMEOUT, and recovery

For the mechanism and rules, see [API: Changing parameters at runtime](api.md#changing-parameters-at-runtime) and [Architecture: Device-coordinated reconfiguration](architecture.md#device-coordinated-reconfiguration). Common symptoms:

| Symptom | Cause and fix |
|---|---|
| RX/TX calls return TIMEOUT | A temporary interruption during reconfiguration. Retry with the same handle (an interval of about 1 ms is recommended), advancing partial sends by accepted; a continuous TX producer should keep calling send rather than waiting for configure to return. An explicit STOPPED or other error must not be treated as a timeout and retried forever |
| Reconfiguration returns BUSY | There is an unreleased RX lease, or you tried to change `rx_mask` / `tx_mask` or the USB IQ bit width while streams exist; stop and destroy all streams first |
| RX shows RECONFIGURED / DISCONTINUITY, or a zero signal is received | A new data segment has started; downstream should reset its demodulation / filter state. Host-side drops during reconfiguration are counted in `host_dropped_samples`, not DEVICE_OVERFLOW; when an active TX resumes, 4 transport blocks of zero IQ are inserted (`tx_padding_samples`) |
| All streams stop after a failed reconfiguration | The core stops both directions and invalidates the configuration cache. Save `och_last_error`, end the I/O threads, destroy the streams, run `rf.reinit` successfully if needed, then configure and recreate the streams; do not loop on start with the old streams |
| Reconfiguration takes longer than usual | Measured worst cases are about 873 ms for a bandwidth change and about 1057 ms for rewriting the TX FIR; typical values are not upper bounds |
| A separate `och config` / `och set` cannot change a device that is streaming | The device is exclusive and there is no cross-process control service; use the C/C++ interface inside the application process. See the [reconfiguration example](../examples/reconfigure.cpp) |

## FIR file or clock rejected

First check the current state and the fir_upload=1 / clock_config=1 capabilities with och fir status, och clock status, and och get system.info. FTR coefficients must be signed16 decimal integers and the tap count a multiple of 16; the two columns are always TX,RX. Files containing RTX/RRX/BWTX/BWRX metadata are rejected by default; only add --coefficients-only when you explicitly want to ignore the accompanying clock/bandwidth, and do not use it to pretend a complete frequency-response configuration was applied.

Valid text does not mean the current hardware clock budget is sufficient. TX factor 1 allows at most 64 taps; other combinations are also limited by the ADC/DAC/Fs ratio. Adjust the design to the target sample rate. The typed API in the same process can coordinate pausing/resuming existing streams, while the CLI still needs exclusive access to the device. A failed SDK call returns the recovery state; with rollback=failed / applied=unknown, first exit and destroy all streams, run rf.reinit successfully, then reconfigure and recreate the streams. Do not keep using an unknown RF state.

The canonical set rx.lo-frequency 1G uses Hz, while the compatibility form set rx_lo_freq 1000 uses the older SDK's MHz. If the unit is wrong, look it up in och commands first instead of guessing by trying several orders of magnitude.

## Relationship to third-party software

GNU Radio (gr-soapy), SDR++, SDRangel, and the SoapySDR Python / C++ APIs can use OmniCoreSDR through the SoapySDR plugin (driver name `omnicore`). The plugin has to be built from source and is not included in the installer; see [SoapySDR Plugin](soapy.md), and its "Troubleshooting" section for plugin-related problems. MATLAB uses its own toolbox (not through SoapySDR); for problems see [MATLAB Toolbox](matlab.md#troubleshooting).

When running through the plugin, the "Runtime reconfiguration" section above applies as well: changing the TX LO, bandwidth, or sample rate briefly pauses the transport; in full duplex this leaves a gap in RX, which the plugin reports as OVERFLOW and then re-tags with time. Plugin writes wait for the pause to end within the timeout given by the caller, so gr-soapy only reports `TIMEOUT` when the pause exceeds 100 ms.

When external PPS is enabled with no input, it shows `no_signal` and the frequency is invalid. For hardware timestamps and timed RX / TX see [Hardware Timestamps](timestamps.md), for PPS disciplining of the 40 MHz see [PPS](pps.md), and for multi-board sync see [Multi-Board Sync](multi-board-sync.md). A board can only be opened by one process at a time, so several programs cannot use it simultaneously. The first-time driver installation by the Windows installer still needs to be tested on clean Windows hosts and under different security policies.
