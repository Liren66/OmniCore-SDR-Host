# Command-Line Reference

The single entry point is `och`. Global options:

| Option | Meaning |
|---|---|
| `--serial SERIAL` | Select a device; without it, only a single matching device is allowed |
| `--json` | Print JSON results on stdout, and event / error JSON lines on stderr |
| `--no-color` | Turn off color; setting the environment variable `NO_COLOR` also works |
| `--help` | Show help |

## Terminal output and scripting

The default output is meant for people and is organized in three levels:

1. **Title**: one full-width title line per functional area, e.g. `DEVICE`, `RF CONFIGURATION`, `RX STREAM SUMMARY`.
2. **Group**: subsections under a title, e.g. `Identity`, `Throughput`, `Faults`.
3. **Field**: labels are left-aligned and values all start at column 31; long values wrap at word boundaries and stay aligned with the value column.

Conclusions use status tags: `OK` (success / no faults), `INFO`, `WARN` (device events or counts that need attention), and `FAIL` (host drops, transfer errors, or command failures). Fault counts are shown in gray when zero and highlighted when non-zero. Explanatory notes always go at the end of each functional area, start with `·`, and are shown in gray. Status returned by the device (such as `get system.status`) is grouped by status group, and groups with short fields are shown in two columns for easier scanning.

`och info` shows on one screen the device identity, the three firmware versions, RF capabilities, time base, and power summary (the values below are illustrative):

~~~text
     ____                  _ ______                 _____ ____  ____
    / __ \____ ___  ____  (_) ____/___  ________    / ___// __ \/ __ \
   / / / / __ `__ \/ __ \/ / /   / __ \/ ___/ _ \   \__ \/ / / / /_/ /
  / /_/ / / / / / / / / / / /___/ /_/ / /  /  __/  ___/ / /_/ / _, _/
  \____/_/ /_/ /_/_/ /_/_/\____/\____/_/   \___/  /____/_____/_/ |_|
  OmniCore Host 0.8.0  |  OmniCoreSDR control and IQ streaming  |  ABI 1

━━ DEVICE ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  Identity ───────────────────────────────────────────────────────────────────
    Name                      OmniCoreSDR
    Serial                    OMNICORE5A3C91D2
    Driver                    libusbK
    USB link                  5.0 Gbit/s  SuperSpeed

  Firmware ───────────────────────────────────────────────────────────────────
    Zynq SDK                  SDK_CMD_1.2_20261107_SYNC
    FPGA IP                   CH32H417_IP 1.3  (framed 16 KiB, hardware time)
    CH32 bridge               1.0.1  (bootloader 1.0.0)

  RF ─────────────────────────────────────────────────────────────────────────
    Channels                  2 RX / 2 TX
    Max sample rate           61.440 MSPS per channel

  Time base ──────────────────────────────────────────────────────────────────
    State                     Valid
    PPS source                Internal, AD9361 40 MHz
    PPS count                 1,284
    Now                       39,462,912,000  (1284.6 s at 30.720 MSPS)

  Power ──────────────────────────────────────────────────────────────────────
    USB-C                     3000 mA available

  · Link speed is negotiated; RX and TX share one USB link and sample clock.
~~~

While a streaming command runs, each event takes one line: relative time, severity, source, and a one-sentence description; events with hardware time add the sample time as `t=`:

~~~text
    +1.092s  [WARN]  RX GAP      FIFO overflow, next block GAP  t=33,521,664
    +2.001s  [ OK ]  TX ACK      Burst complete  t=61,440,000
    +2.310s  [WARN]  TX LATE     Late start, burst dropped  t=30,720,000
    +3.500s  [WARN]  TIME        Rate 30.720 -> 15.360 MSPS, time invalid
~~~

<p align="center"><img src="images/cli-status.png" width="640" alt="Grouped two-column view of get system.status"></p>

The output style is chosen automatically based on the terminal:

| Situation | Output |
|---|---|
| stdout / stderr is an interactive console that supports VT sequences | Color and Unicode rules; streaming commands refresh a progress bar line every second |
| `--no-color` or `NO_COLOR` set | Unicode layout kept, no color |
| Output redirected to a file or pipe | Plain 7-bit ASCII, at most 78 columns per line, no progress refresh |
| `--json` | JSON only, no titles, color, or progress |

The body, configuration, and final statistics go to stdout; run state (`RUN` / `STOP`), asynchronous events, progress, and errors go to stderr. A streaming command shows the session, transport, and RF configuration at the start; at the end it first prints a throughput summary table per direction (measured MB/s, theoretical wire MB/s, ratio, and fault count), then the grouped statistics for RX / TX:

| Group | Contents |
|---|---|
| Overview | Channels × sample rate, run time, samples delivered (RX) / accepted (TX), GAPs accepted within the start-up window |
| Throughput | Average USB throughput, ratio to the theoretical wire rate, data volume, host queue peak |
| Faults | RX: device overflows (GAP), host drops, header errors; TX: device underflows, host starvation, zero-padded samples; both: USB transfer errors and event queue drops |

Event source tags: `RX GAP`, `RX LATE`, `TX ACK`, `TX LATE`, `TX UNDERRUN`, `TX BLOCK`, `TIME`, `HOST`, `USB`, `TX FEED`, `RECONFIG`, `DEVICE`. Sequence numbers, epochs, and host timestamps are not shown in the terminal; read them with `--json` when needed.

Under overload the device may report thousands of events per second. The terminal lists only the first 3 events of each kind individually, then summarizes each kind in one line per second (e.g. `TX UNDERRUN  +3,072 more (summarized)`), and at the end the `Events` field gives the total and the number listed individually; `--json` still outputs every event. When the RX + TX wire demand exceeds the roughly 533 MB/s of the UHSIF bus between the FPGA and the CH32, a `WARN` is shown before the stream starts.

The default text layout may improve from version to version; automation should use `--json` or the public C/C++ API. The result fields and structure of `--json` stay compatible with older versions. When reading JSON, you still need to handle standard output, standard error, and the process exit code separately; having events does not necessarily mean the command failed.

### USB2 sample rates and stability verdict

`info` uses the same groups on USB2 and USB3. USB2 shows `480 Mbit/s High-Speed (USB 2.0)`, and `Max sample rate` still means the RF capability. For measured sample rates, test hosts, and durations, see [Benchmarks](benchmarks.md#usb-20-high-speed); `info --json` keeps `usb2_reference` for scripts.

At stream start, `USB link`, `RX + TX wire demand`, and the reference value matching the configuration are shown. Exceeding the reference sample rate gives a `WARN`, but parameters are not changed automatically and the SDK's RF range is not restricted. The single-channel reference was only measured on CH1; CH2-only, asymmetric full duplex, and other firmware combinations are shown as untested. Even measured points cannot guarantee the result on another host.

The stability verdict allows **RX overflows within the first second**: with framed transport, each overflow produces one `RX GAP` event and the next block recovers automatically without restarting RX. The number of GAPs within the window (no more than the device overflow count) is counted in `Startup gaps accepted`. No raw statistics are reset; GAPs after 1 second, and header errors, underflows, host drops, transfer errors, lost events, TX starvation, or zero padding at any stage still cause a warning / failure. This "stable after start-up" verdict does not mean the start-up data is lossless; the number of lost samples is given exactly by `device_dropped_samples` in the metadata.

A `WARN` is also shown when the observation time is at least 1 second and there are no remaining error counts, but throughput is below 98% of theoretical; the average includes start/stop overhead, so short tests in particular must be interpreted together with their duration. The exit code still only says whether the command completed and cannot replace these acceptance conditions. The start-up window is timed from the return of the stream start call, and event times are the host observation time when the CLI picked up the event.

`bench --fail-fast` is for scanning for failing settings: after the first second, if the RX device overflow count (GAP blocks in the data path) increases twice no more than 2 seconds apart, it stops the stream early and normally, saves the statistics, and returns exit code `3`. It looks at the GAP flag in data blocks rather than `RX GAP` events: when TX underflow events are dense, the FPGA event FIFO overflows and drops events, but the flags in data blocks are never lost. Overflows within the start-up window do not trigger an early stop. `stopped_early=true` and `stability.pass=false` in the stdout JSON indicate that the requested duration was not completed; candidates without consecutive overflows still run for the full duration. The USB2 scan script enables this option by default.

The JSON keeps the old fields; the additions are:

| Location | New field | Meaning |
|---|---|---|
| `info` stdout | `sample_rate_scope: "rf_capability"` | The meaning of the existing `max_sample_rate`; the value is unchanged |
| `info` stdout, USB2 with matching firmware | `usb2_reference` | `scope: "single_host_observation"`, test date, and a `profiles` array; each row contains `mode`, `iq_bits`, `rx_mask`, `tx_mask`, `sample_rate_hz`, `test_seconds` |
| Stream stderr, beyond the measured reference | `type: "warning"`, `code: "usb2_reference_exceeded"` | Also provides `reference_rate_hz`, `test_seconds`, `scope` |
| Stream stderr events | `host_monotonic_ns`, `observed_seconds` | Host event timestamp, and seconds since the start of the run when the CLI picked up the event; the latter is observation time, not when the hardware event occurred |
| Stream stderr events (0.6.1) | `count` | Number of device events the record stands for; greater than 1 for underflow/GAP summaries merged by the firmware |
| Stream stdout (0.6.1) | `device_events` | Sum of `count` over all event records; `events` is still the number of records |
| Stream stdout | `stability` | `startup_window_seconds`, `policy`, `accepted_startup_overflows`, remaining fault counts per direction, and `pass` for this observation; the original `rx` / `tx` statistics are kept in full |

## Discovery and control

| Command | Meaning |
|---|---|
| och find | Enumerate devices; does not start transmitting |
| och info | Query identity, capabilities, and USB-C power information |
| och power status --json | Read the HD3SS3220 and report the USB-C power state |
| och config | Query the device's system_status state |
| och config --sample-rate 30.72M --lo-frequency 1G | Apply RF from the default configuration plus the given parameters; does not start TX |
| och commands --json | List all canonical commands, units, ranges, and SDK equivalents offline |
| och get system.status | Query the current system state |
| och get usb.irq | Query bridge error/recovery counts |
| och get rf.register 0x037 | Read an AD9361 register |
| och set rx.lo-frequency 1G | Set the RX LO to 1 GHz |
| och set tx1.attenuation 10 | Set TX1 attenuation to 10 dB |
| och get rx.dc-correction | Query the AD9361 RF/baseband DC tracking correction state |
| och set rx.dc-correction on | Turn on RF/baseband DC tracking correction for both RX channels |
| och set rx.iq-correction on | Turn on quadrature tracking correction for both RX channels |
| och set rx.lo-source external | Use the external LO input for RX; default internal |
| och set tx.lo-source external | Use the external LO input for TX; default internal |
| och set clock.ref-source external | Enable ADF4002 disciplining to external 10 MHz; the AD9361 input is still 40 MHz |
| och get clock.ref-status | ADF4002 configuration and observability; lock and LED state unknown |
| och action clock.ref-test | D5 blinks three times and the source selection is restored; needs manual observation, no 10 MHz required |
| och action usb.clear | Request clearing of latched errors without erasing historical statistics |
| och fir status | Query the FIR configuration and verification state of both directions |
| och fir rx load filter.ftr | Load the RX FIR; the RX FIR stays off after a successful load |
| och fir both on | Turn on the RX/TX FIRs in one transaction |
| och clock status | Query the actual reference clock and the six-stage clock chains of both directions |
| och clock sample-rate 30.72M | Compute and apply the shared sample rate, then read it back; when no exact clock solution exists, the nearest usable rate is used with a `WARN` |
| och selftest | Stop the data path and run the device's RX/TX digital self-test |
| och help | Show commands and arguments |
| och version --json | Query the software and ABI versions |
| och firmware | Query the CH32 bridge firmware mode and version (does not access the SDK) |
| och firmware update FILE.ocfw | Update the bridge firmware over USB; see [Bridge Firmware Update](firmware-update.md) |
| och firmware boot / enter | Leave / enter the bridge bootloader |

Canonical names use lowercase dot-separated groups, e.g. rx.lo-frequency, tx1.attenuation. get/set/action validate arguments against the [full catalog](commands.md) and convert them to SDK units: frequencies in Hz, gain/attenuation in dB, and DDS scale as a normalized ratio. Single-value read-backs are also converted to canonical units; structured status content keeps the firmware fields. In JSON, reply is the canonical result, raw_reply keeps the original response, and sdk_command is the command actually sent. PPS uses `pps.input-enabled` and `pps.status`, and the 40 MHz tuning mode and CH32 loop state use `clock.vcxo-status`; older firmware lacking `pps_monitor=1`/`vcxo=1` reports unsupported explicitly; see [PPS](pps.md). do is a compatibility alias for action.

get/set with the original SDK names remain as raw pass-through for compatibility: in och set rx_lo_freq 1000, 1000 is MHz, and in och set tx1_attenuation 10000, 10000 is mdB. The raw interface does not convert number suffixes or returned units. Only one command is allowed at a time, and batch injection through newlines is rejected; when the firmware rejects a command, failure is returned. New programs should prefer the canonical names.

`rx.dc-correction` / `rx.iq-correction` accept `on/off`, `true/false`, `1/0`, and require SDK `rx_correction=1`; setting one keeps the other and verifies the read-back, and the JSON `raw_reply` is the raw firmware state queried after the setting. Both are the AD9361's native RX tracking; for the semantics see [API: RX DC and IQ correction](api.md#rx-dc-and-iq-correction).

For the FIR and clock file formats, transaction boundaries, and actual clock limits, see [FIR and Clocks](fir.md). These operations do not start data streams and do not change the CH32 firmware. Every CLI operation that needs the device takes exclusive ownership of it and cleans up the old USB session; find, commands, help, and version do not open the device.

Reference and LO sources accept `internal/external` or `0/1`, internal by default. The nominal RF range of the external LO is 70 MHz..4 GHz, with a physical input at twice that; in the external state, frequency commands only declare the frequency and cannot tune the signal source. Returning to internal restores the previous internal PLL frequency. The ADF4002 MUXOUT only connects to D5, so software cannot read back lock or whether the LED is lit, and `locked=-1` / `led_on=-1` must stay unknown. `gpio_verified=1` from the communication self-test only proves the GPIO pad read-back; watch D5 blink three times to supply evidence of the chip responding. See [Reference Clock and External LO](reference-lo.md).

find only enumerates the matching devices on the system (the same on Windows, Linux, and macOS) and does not open the USB pipes to probe the link speed. A speed that has not been queried is shown as unknown in the default text; for compatibility, usb_speed_mbps in JSON is still 0, which also means not queried. Run info when you need to confirm the actual link; do not take a 0 from the enumeration stage as the transfer speed or as evidence of a disconnect.

## USB-C power information

och power and och power status are equivalent; both support --serial and --json. The existing JSON fields of och info are unchanged, with a power object added. och find still only enumerates devices and does not read the power log.

**Since bridge firmware 1.0.1**: every query sends `typec?` through EP3 OUT and the bridge firmware reads the HD3SS3220 on the spot; source is typec_query, live is true, the text shows `USB-C`, repeated runs give consistent results, and it takes about 0.1 s.

**Bridge firmware 1.0.0**: the information comes from the HD3SS3220 **boot log** on the existing EP83; source is startup_log, live is false, and the text shows `USB-C at startup`. It is an observation at boot, not a live poll of the current connection, and not a measurement of actual current draw or supply voltage. supports_3a is only true or false when the log is complete and decodes to a valid UFP connection; with no log, an invalid state, or not UFP it is JSON null and the text shows unknown, and unknown must not be taken to mean 3 A is unsupported.

By default the query waits at most 500 ms for the existing log. The boot log may already have been read by another program; when it is missing, the device is not reset and no firmware update is required to regenerate it. Query results are not cached on disk, and results from an old computer/old connection cannot be passed off as the current device state.

raw_08/raw_09/raw_0a keep the register values, attached_state/current_detected keep the decoded fields, and source_current_ma is the current advertisement detected at boot. USB default is not converted into a fixed number of milliamps; in that case source_current_ma is null. Reading EP83 consumes the debug bytes, and the cache only exists in the device handle of the current process, so it is not kept for the next CLI run after the handle is closed or the process exits; it is therefore possible for a first info to show 3 A support and a later power status to show unknown, which must not be interpreted as the 3 A capability disappearing.

The host cannot use this to determine the physical level of the board's LED pin or whether it is lit, nor prove that the supply can actually sustain 3 A under the current load. After replugging, check again; if the firmware produced no new log, keep the unknown/non-live limitations.

## Streams and standard arguments

| Argument | Meaning |
|---|---|
| --sample-rate 61.44M | Per-channel sample rate in Hz; shared by RX/TX |
| --channels 1 / 2 / 1,2 | Physical channels; dual channel by default |
| --rx-channels / --tx-channels | Per-direction selection for bench/config |
| --lo-frequency 1G | Set both RX/TX LO, Hz |
| --rx-lo-frequency / --tx-lo-frequency | Set each LO separately, Hz |
| --bandwidth 18M | Set both RX/TX analog bandwidth, Hz |
| --rx-bw / --tx-bw | Set each bandwidth separately |
| --gain 30 | Manual gain of both RX in dB; switches to manual by default |
| --agc manual/fast/slow/hybrid | Receive gain mode; explicit arguments take priority |
| --attenuation 10 | Attenuation of both TX in dB |
| --format packed12/sc16/fc32/sc8 | File or application format; rx/tx default to sc16; sc8 needs the 8-bit wire |
| --iq-bits 12/8 | USB wire precision, 12 by default; 8 needs bench --rx --tx and channels 1,2 in both directions |
| --seconds 10 | Duration; 0 means until Ctrl+C |
| --buffer-mib 64 | Host queue size per direction in MiB, at most 512 |
| --queue 32 | Parallel USB requests per direction |
| --block 196608 | USB request size in bytes; must be a multiple of 16384 (one framed block) |

Decimal numbers accept k/K, M, and G, e.g. 30.72M, 1e9. Typos, invalid suffixes, non-finite numbers, duplicate options, invalid channels, and so on are rejected explicitly, and a valid numeric prefix is never silently truncated.

The old options --rate, --frequency, --rx-freq, and --tx-freq remain compatible with --sample-rate, --lo-frequency, --rx-lo-frequency, and --tx-lo-frequency respectively; passing both the new and old name of the same argument reports a conflict, so the order of arguments never decides the result. Help and new examples only use the canonical names.

Streaming commands use the command-line arguments plus the core's default RF configuration: 30.72 MSPS and two channels by default. When you need specific LO, bandwidth, gain, and so on, pass them with every streaming command, or keep a configuration object in your own program. Hardware FIR coefficients and on/off state already loaded are kept, so you cannot assume every stream bypasses the filter automatically. After the sample rate changes, the frequency response in Hz represented by the existing FIR coefficients changes with it; invalid combinations fail explicitly.

### RX

~~~powershell
och rx --sample-rate 30.72M --channels 1,2 --seconds 5 --file iq.sc16 --format sc16 --rx-lo-frequency 1G
~~~

Only the payload is written to the file; the 16 B header of each 16 KiB block is stripped. A separate iq.sc16.metadata.jsonl is written, recording line by line:

- capture: format, configuration, `hardware_timestamps` (whether the device provides hardware time).
- boundary: file position file_byte/file_sample, epoch/sample_index, flags, host drops, device drops (exact value derived from the block header time difference), `time_ticks` (hardware time of the first sample of this segment), and `tick_rate`.
- end: final statistics; an abnormal termination may lack this line, and the existing boundaries can still be read.

Files have no extra container header. sc16/fc32 are little-endian; dual channel is ordered I1,Q1,I2,Q2, and single channel is I,Q of the selected physical channel. An existing file is only overwritten with --overwrite.

### TX

~~~powershell
och tx --sample-rate 61.44M --channels 1 --tone 1M --amplitude 0.25 --seconds 10
och tx --sample-rate 30.72M --channels 1,2 --file iq.sc16 --format sc16
och tx --sample-rate 30.72M --channels 1,2 --file iq.sc16 --format sc16 --loop --seconds 30
~~~

A normal file is sent until EOF by default, and --loop repeats the file. The file must consist of complete interleaved IQ frames; metadata.jsonl or a raw USB dump with block headers kept must not be transmitted as IQ. A capture file with several discontinuous epochs is still played in file order, without reconstructing the original time gaps between captures.

TX prefills at least four complete USB blocks; a continuous source runs its producer before starting, preparing min(32 MiB, half the host queue) of wire data as scheduling headroom, then explicitly starts output. A finite file that reaches EOF early can start with the data already prefilled. A non-looping file shorter than four blocks is rejected; use --loop to repeat-fill it.

`och tx` sends an untimed continuous stream; to transmit bursts at specific sample times, use `och_send_burst` from the C API, see [Hardware Timestamps](timestamps.md). A timed continuous source closes the hardware immediately when time is up and cancels the remaining queued data, without waiting for the queue to drain before ending. Only a file EOF flushes explicitly, and the necessary transport-block zero padding is reported as tx_padding_samples. The number of samples accepted into the host queue is not the number played over RF; neither way of stopping provides an exact RF end time.

**Trailing underflow after a finite file's EOF**: after flush completes, the device keeps draining the FIFO, and TX underflow / host starvation notifications may appear before the hardware is closed; the CLI reports them as they are. They do not mean samples were lost in the middle of continuous data; for why, see [Architecture: Explicit TX start and stop](architecture.md#explicit-tx-start-and-stop). Acceptance results for continuous-stream benchmarks and finite file playback should be reported separately.

The test sine is cos+j sin, with the same test code on both channels. To keep the period seamless and sustain throughput, arbitrary frequencies with very long periods are quantized to a period of at most 65536 points, and actual_Hz is reported on stderr; 1 MHz at 30.72/61.44 MSPS has an exact integer period. Amplitude is 0..1, where 1 corresponds to the 12-bit peak of 2047; default 0.25.

### Benchmark

~~~powershell
och bench --rx --tx --sample-rate 30.72M --channels 1,2 --seconds 30 --json
och bench --rx --tx --sample-rate 61.44M --rx-channels 1 --tx-channels 2 --seconds 30
~~~

You must choose --rx, --tx, or both. bench uses the native wire format: packed12 by default for 12-bit; `--iq-bits 8` defaults to sc8 and requires `--rx --tx --channels 1,2`. There is no disk I/O and no floating-point conversion. usb_MBps is decimal MB/s including session start/stop overhead; it is not a peak taken over a steady window only, and the byte rate alone cannot prove the RF waveform is correct. `och get usb.iq-bits` queries the format, and `och set usb.iq-bits 8|12` changes it after streams are stopped; channel and source restrictions are verified by the SDK, and normal streams use `--iq-bits` to coordinate the configuration automatically. See [8-bit extension](iq8.md).

## Exit status

| Exit code | Meaning |
|---|---|
| 0 | Command completed |
| 1 | Device, transport, or runtime error |
| 2 | Invalid arguments (device not accessed) |
| 3 | `bench --fail-fast` stopped early because of consecutive RX overflows; cleaned up with statistics kept |
| 130 | Interrupted by Ctrl+C; cleaned up |

By default, EVTs/discontinuities do not stop the stream automatically; `bench --fail-fast` is the exception described above. Whether the job passed should be judged from the statistics on discontinuities, underflows, header errors, and queue losses, not from the exit code alone.

## RX RSSI

RSSI reporting is **off by default** and returns to off after a power cycle; run `och rssi on` first to enable it (`och rssi off` turns it off, or use `och set rx.rssi-enabled on|off`). When it is not enabled, `och rssi` prints a notice and exits with code 1. `och rssi` reads the symbol RSSI of RX1 and RX2. The unit is relative dB, larger is stronger, and it is **not calibrated input power**. When the ENSM is not in the receive state it shows `--`, and `null` in JSON. This board runs 2R2T, so even when USB uploads only one channel, RSSI can still be read for both. `--count N` sets the number of reads (0 reads until Ctrl+C), and `--interval 0.5` sets the read interval in seconds. `och get rx.rssi` reads once, in the same format as other query commands.

```powershell
och rssi on
och rssi
och rssi --count 0 --interval 0.2
och rssi --count 10 --json
```

On new SDKs, `rx_rssi?` takes about 1.1 ms per read. On older SDKs, OCH parses it from `system_status?`, about 190 ms per read. See [Device Protocol: Control UART](protocol.md#control-uart).

## RGB and fan control

`rgb.enabled` / `rgb.status`, `fan.enabled` / `fan.status`, and `core.temperature` control and query the RGB status LED, fan temperature control, and Zynq core temperature, without stopping IQ streams. For defaults, status colors, the fan control policy, and firmware requirements, see [Board Control](board-control.md).
