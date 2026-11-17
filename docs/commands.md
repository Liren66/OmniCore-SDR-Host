# OmniCore Host Command Catalog

Canonical commands use lowercase English, dot-separated groups, and hyphens: the sample rate is always written `sample-rate`, and the RF LO always contains `lo`. `och commands` shows the full catalog with argument ranges and operation types, and `och commands --json` provides the metadata for scripts; neither command needs a connected device.

```powershell
och get rx.sample-rate
och set clock.sample-rate 30.72M
och set rx.lo-frequency 2.4G
och set tx.lo-frequency 2.4G
och set tx1.attenuation 10.25
och set rx1.gain-mode manual
och set rx1.gain 30
och get rx.dc-correction
och set rx.dc-correction on
och set rx.iq-correction on
och get clock.chain
och get rf.register 0x037
och action usb.clear
```

`get` queries, `set` sets, and `action` performs an action. Some commands support only one of these operations. Multiple arguments are separated by spaces in the order given in the tables. Set operations never start transmitting automatically; TX is only requested on when `tx.enabled on` is set explicitly, and the USB data source must also be prefilled first. For normal capture/transmit, use `och rx`, `och tx`, and `och bench`, which let the core coordinate buffering and start order.

All canonical frequencies and bandwidths are in Hz, gain/attenuation in dB, and DDS amplitude as a ratio in -1..1. Numbers accept decimal, fractional, and scientific notation with `k`/`K`, `M`, `G` suffixes; for example `30.72M` equals `30720000`. NaN, Inf, trailing characters, embedded spaces, the wrong number of arguments, and values that do not match the step are rejected. Only register addresses and bridge control bit masks additionally accept `0x` hexadecimal.

RX/TX share the sample clock. `rx.sample-rate`, `tx.sample-rate`, and `clock.sample-rate` all affect both directions; there are no independent sample rates per direction. The usable rates are further limited by FIR interpolation/decimation and the ADC/DAC clock chain. All three canonical settings use the new SDK's `clock_sample_rate=` path for clock chain generation and verification; the corresponding old raw settings keep their original behavior. Use the dedicated `och fir` command to load FIRs and the dedicated `och clock` command for custom clock chains; do not assemble upload/commit transactions by hand.

The main scalar fields returned by `get`/`set` are converted to canonical units and use canonical names. For example, the SDK's `tx1_attenuation=10250` is shown as `tx1.attenuation=10.25`. Multi-field status and extra diagnostic content keep the SDK format. Returned values are what the device reports and may be quantized by the hardware; do not assume they always equal the requested value.

## RF and channels

`get/set` in the table correspond to the same SDK name with the suffix `?`/`=`; get-only commands provide no setter.

| Canonical name | Operation | Arguments, units, and constraints | SDK name |
|---|---|---|---|
| `rx.sample-rate` | get/set | Integer Hz, 1..61,440,000; shared clock, FIR determines the effective lower limit | Query `rx_samp_freq?`, set `clock_sample_rate=` |
| `tx.sample-rate` | get/set | Same as above; changes the clock for both directions | Query `tx_samp_freq?`, set `clock_sample_rate=` |
| `rx.lo-frequency` | get/set | Integer Hz, 70,000,000..6,000,000,000 | `rx_lo_freq`, SDK unit MHz |
| `tx.lo-frequency` | get/set | Same as above | `tx_lo_freq`, SDK unit MHz |
| `rx.lo-source` | get/set | `internal`/`0`, `external`/`1`; external nominal RF limited to 4 GHz, physical input is 2× | `rx_lo_source` |
| `tx.lo-source` | get/set | Same as above; selected independently from RX | `tx_lo_source` |
| `rx.rf-bandwidth` | get/set | Integer Hz, 200,000..56,000,000 | `rx_rf_bandwidth` |
| `tx.rf-bandwidth` | get/set | Same as above | `tx_rf_bandwidth` |
| `rx.dc-correction` | get/set | Boolean; RF and baseband DC tracking correction on both RX channels | `rx_dc_correction` |
| `rx.iq-correction` | get/set | Boolean; quadrature tracking correction on both RX channels | `rx_iq_correction` |
| `rx.channels` | get/set | `off`/`0`, `ch1`/`1`, `ch2`/`2`, `both`/`3`/`1,2` | `rx_channel` |
| `tx.channels` | get/set | Same as above | `tx_channel` |
| `rx1.gain-mode` | get/set | `manual`/`0`, `fast`/`1`, `slow`/`2`, `hybrid`/`3` | `rx1_gc_mode` |
| `rx2.gain-mode` | get/set | Same as above | `rx2_gc_mode` |
| `rx1.gain` | get/set | Integer dB, -3..76; the current band may limit it further; set in manual gain mode | `rx1_rf_gain` |
| `rx2.gain` | get/set | Same as above | `rx2_rf_gain` |
| `tx1.attenuation` | get/set | 0..89.75 dB in 0.25 dB steps | `tx1_attenuation`, SDK unit mdB |
| `tx2.attenuation` | get/set | Same as above | `tx2_attenuation`, SDK unit mdB |
| `tx.source` | get/set | `dds`/`0`, `usb`/`1`; stop TX before changing | `tx_source` |
| `tx.enabled` | get/set | Boolean; explicitly controls the transmit gate | `tx_enable` |
| `rx.fir-enabled` | get/set | Boolean; enables the loaded FIR; prefer the complete FIR transaction | `rx_fir_en` |
| `tx.fir-enabled` | get/set | Same as above | `tx_fir_en` |
| `rx.fir-profile` | get/set | Compatibility built-in example profile `0`; loading disables the corresponding FIR | `rx_fir_config` |
| `tx.fir-profile` | get/set | Same as above | `tx_fir_config` |

Booleans accept `off`/`false`/`0` and `on`/`true`/`1`. A channel mask is not a channel count: `2` means the second channel, and only `3` or `both` means both channels. When TX is off, the SDK keeps the actual hardware at maximum attenuation, and the attenuation query returns the saved transmit target value.

The two RX correction commands require `rx_correction=1` in `system_info?`; setting one keeps the other and verifies by read-back. `off` only stops tracking updates and does not clear existing coefficients; for the full semantics see [API: RX DC and IQ correction](api.md#rx-dc-and-iq-correction). Raw SDK write commands require streams to be stopped; applications that stream continuously should use the in-process C/C++ setters.

## DDS

TX1 and TX2 each have two tones, and all 12 properties support get/set. Frequencies are set as integer Hz; the host checks 0..4,294,967,295, and the SDK additionally checks against the current TX sample rate that it does not exceed the Nyquist frequency. Phase is the I-path phase, with the Q path kept at I + 270°; 0..360° is allowed in 0.001° steps, and 360° wraps to 0°. scale is -1..1 in 0.000001 steps; the sum of two tones may clip.

| Canonical name | SDK name | Canonical unit |
|---|---|---|
| `dds.tx1.tone1.frequency` | `dds_tx1_tone1_freq` | Hz |
| `dds.tx1.tone1.phase` | `dds_tx1_tone1_phase` | Degrees |
| `dds.tx1.tone1.scale` | `dds_tx1_tone1_scale` | Ratio; the SDK uses millionths |
| `dds.tx1.tone2.frequency` | `dds_tx1_tone2_freq` | Hz |
| `dds.tx1.tone2.phase` | `dds_tx1_tone2_phase` | Degrees |
| `dds.tx1.tone2.scale` | `dds_tx1_tone2_scale` | Ratio; the SDK uses millionths |
| `dds.tx2.tone1.frequency` | `dds_tx2_tone1_freq` | Hz |
| `dds.tx2.tone1.phase` | `dds_tx2_tone1_phase` | Degrees |
| `dds.tx2.tone1.scale` | `dds_tx2_tone1_scale` | Ratio; the SDK uses millionths |
| `dds.tx2.tone2.frequency` | `dds_tx2_tone2_freq` | Hz |
| `dds.tx2.tone2.phase` | `dds_tx2_tone2_phase` | Degrees |
| `dds.tx2.tone2.scale` | `dds_tx2_tone2_scale` | Ratio; the SDK uses millionths |

## Clocks, system, and USB

`och get usb.iq-bits` / `och set usb.iq-bits 8|12` map to `iq_format?` / `iq_format=8|12`. 8-bit needs a matching SDK/FPGA, both RX/TX masks set to 3, and the USB TX source; both directions must be fully stopped before changing the format. Normal streams coordinate this setting automatically with `--iq-bits 8`. See [8-bit extension](iq8.md).

| Canonical name | Operation | Arguments and meaning | SDK name |
|---|---|---|---|
| `clock.chain` | get | Reference clock and the 6-stage RX/TX clock chains, Hz; complete chain settings go through `och clock` | `clock_chain?` |
| `clock.sample-rate` | get/set | Integer Hz, 1..61,440,000; shared chain generation and verification, limited by FIR | Query `rx_samp_freq?`; set `clock_sample_rate=` |
| `clock.ref-source` | get/set | `internal`/`0` free-running, `external`/`1` disciplined by the ADF4002 from external 10 MHz; the AD9361 still runs at 40 MHz | `clock_ref_source?` / `=` |
| `clock.ref-status` | get | Reference configuration, GPIO read-back; with PPS only, LOCK comes from the CH32 loop, with 10M+PPS it is a ±1 ppm frequency LOCK, hidden without PPS | `adf4002_status?` |
| `clock.ref-test` | action | D5 blinks three times, then the source selection is restored; needs manual confirmation and cannot automatically prove chip communication/lock | `adf4002_self_test!` |
| `pps.input-enabled` | get/set | `on`/`off` or `0`/`1`, off by default; with the internal reference, the CH32 PA4 DAC disciplines the 40 MHz from PPS; with external 10 MHz it is for reference only | `pps_input_en?` / `=` |
| `pps.status` | get | Input state, measured DATA_CLK frequency/offset; no valid frequency without input | `pps_status?` |
| `clock.vcxo-status` | get | 40 MHz tuning mode (0 neither / 1 10M / 2 PPS / 3 10M+PPS), ownership of ADF4002 and PA4, CH32 PPS loop state | `vcxo_status?` |
| `host.protocol` | get | Control wrapper version and capabilities | `host_protocol?` |
| `system.help` | get | The SDK's built-in full command help, in original units | `help?` |
| `system.info` | get | Firmware, channels, data formats, recovery capabilities | `system_info?` |
| `system.status` | get | RF, temperature, RSSI, RX/TX state, fault history | `system_status?` |
| `rf.register` | get | One register address, 0..1023, `0x037` accepted | `register?55` etc. |
| `rf.reinit` | action | Reinitialize RF/PL from the configuration saved in the SDK; interrupts transport | `rf_reinit!` |
| `digital.self-test` | action | Run the digital interface test after stopping streams | `digital_self_test!` |
| `usb.data-enabled` | get/set | Two booleans: RX enabled, TX enabled | `usb_data_en?` / `=` |
| `usb.config` | get/set | CTRL0 integer bit mask 0..127, hexadecimal allowed; subject to SDK state checks | `usb_config?` / `=` |
| `usb.rx` | set | Three booleans: enabled, dual-channel, select-channel2 | `usb_rx=` |
| `usb.tx` | set | Four booleans: enabled, dual-channel, select-channel2, run | `usb_tx=` |
| `usb.status` | get | FIFO, errors, throughput estimate, and recovery counts | `usb_status?` |
| `usb.irq` | get | Interrupt state, error clearing, and history | `usb_irq?` |
| `usb.clear` | action | Request asynchronous error clearing; keeps history, does not reset FIFOs | `usb_clear!` |
| `usb.stop` | action | Stop the USB path; in USB TX source mode it also turns TX off | `usb_stop!` |

`usb.rx`, `usb.tx`, and `usb.config` are advanced bridge debugging entry points and cannot bypass the SDK's explicit TX enable, prefill, and state checks. With dual channels, select-channel2 does not take part in selection. RX overflow is handled by the FPGA: the current block is finished with zero padding and the next block carries the GAP flag, from which the core starts a new epoch and reports the exact number of dropped samples; lost IQ data is not recovered.

## Legacy command compatibility

Old SDK names are still kept through the raw command entry point, e.g. `och get rx_samp_freq`, `och set tx1_attenuation 10000`; old actions can be used with `och set 'usb_clear!'`. Old names keep the SDK's original units and are **not converted automatically**:

| Parameter | Canonical command | Old SDK command |
|---|---|---|
| RX LO 2.4 GHz | `och set rx.lo-frequency 2.4G` | `och set rx_lo_freq 2400`, unit MHz |
| TX1 attenuation 10 dB | `och set tx1.attenuation 10` | `och set tx1_attenuation 10000`, unit mdB |
| DDS amplitude 0.25 | `och set dds.tx1.tone1.scale 0.25` | `och set dds_tx1_tone1_scale 250000` |

The SDK's existing compatibility aliases from `ch32_` to `usb_` still belong to the raw command layer; the canonical catalog only publishes `usb.*`. Register queries with arguments currently follow a conservative policy that requires no stream objects; stop and destroy the stream objects before querying. Internal FIR/clock transaction commands such as begin, data, commit, and abort are not in the user catalog; the dedicated API/CLI handles argument validation, ordering, failure handling, and read-back confirmation.

The catalog is implemented in `apps/och/commands.hpp`, which contains each operation, argument count, unit conversion, step, enumeration, and support state; `och commands --json` provides the complete machine-readable catalog. The catalog describes the capabilities of the host entry points and does not replace the runtime checks the SDK performs based on the current hardware state. For the underlying wrapper and FIR/clock transactions, see the [Device Protocol Reference](protocol.md).
