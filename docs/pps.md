# PPS Clock Frequency Monitoring and 40 MHz Disciplining

The PPS counter measures the DATA_CLK frequency of the AD9361 digital interface. This page covers that monitoring and the 40 MHz disciplining; it does not provide UTC time by itself. For loading sample time on PPS and timed RX/TX, see [Hardware Timestamps](timestamps.md). Monitoring is off by default, and the board starts and streams normally without external PPS. On PA4 tuning hardware (SDK `vcxo=1`), when only PPS is used without an external 10 MHz, the CH32 drives the PA4 DAC from these counts to discipline the on-board 40 MHz; see [40 MHz tuning mode](#40-mhz-tuning-mode-sdk-vcxo1) below.

## Usage

```powershell
och get pps.input-enabled
och set pps.input-enabled on
och get pps.status
och get pps.status --json
och set pps.input-enabled off
```

Raw SDK commands: `pps_input_en?`, `pps_input_en=0|1`, `pps_status?`. `system_status?` also includes the PPS state. `pps_monitor=1` in `system_info?` means the SDK is connected to an FPGA with PPS support; `pps_adjustment=0` means there is no adjustment function. Older firmware/bitstreams return unsupported, and no valid measurement is fabricated.

Enabling only unmasks the PPS interrupt and starts the statistics; disabling masks the interrupt and clears the current statistics. The input sampling and counting module inside the FPGA remains in place either way. Turning monitoring on or off does not pause IQ streams, change the sample rate, or switch the reference source. The setting persists across closing/reopening OCH and across `rf_reinit!`; restarting the SDK returns it to off.

## States and data

| Field/state | Meaning |
|---|---|
| `disabled` | Monitoring is off |
| `waiting` | Waiting for two consecutive valid pulses after enabling or reconfiguring |
| `valid` | The most recent complete PPS interval produced a valid frequency |
| `no_signal` | No new pulse for more than 1.5 s after enabling or after the last pulse |
| `invalid_interval` | Abnormal pulse interval or count, including the first recovery after a missed pulse |
| `clock_unavailable` | RF clock not ready; the monitoring setting is kept and measurement is paused |
| `signal_present` | A pulse was received within the last 1.5 s; this does not mean a qualified 1 Hz signal |
| `frequency_valid` | Must be 1 before the frequency and offset can be used |
| `nominal_hz` | Nominal DATA_CLK derived from the current clock configuration, not a measurement |
| `measured_hz` | DATA_CLK frequency obtained from the FPGA period count, assuming the external pulse is exactly 1 Hz |
| `error_ppm` | `(measured_hz / nominal_hz - 1) × 10^6`; the protocol returns `unknown` when invalid |
| `pulse_count` / `invalid_count` | IRQs received / intervals rejected while enabled this time; 32-bit counters that may wrap |
| `age_ms` | Milliseconds since the last pulse; 4294967295 means no pulse yet since enabling/re-measuring |
| `raw_counter` | Raw FPGA count when valid; after correcting the zero origin, `measured_hz = raw_counter + 1` |
| `hardware_timeout` | Timeout flag built into the HDL, for diagnostics only; its threshold depends on DATA_CLK, while the software state uses a fixed 1.5 s |

The physical interface is currently fixed at 2R2T LVDS, with DATA_CLK = 4 × per-channel sample rate. 30.72 MSPS corresponds to a nominal 122.88 MHz and 61.44 MSPS to 245.76 MHz; selecting only one RX channel over USB does not change this relationship. The monitored value is not a direct reading of the on-board 40 MHz reference clock.

The first PPS only establishes the starting point and publishes no frequency. The PS timer only checks that the IRQ interval is within 0.9–1.1 s, and the hardware count must also be within ±10% of the nominal frequency; the PS receive time is not used to compute the precise frequency. The next consecutive qualified PPS makes it valid. When the input is lost, publishing the stale frequency stops immediately, with `measured_hz=0` and `error_ppm=unknown`; zero is an invalid placeholder and does not mean 0 Hz was actually measured. After changing the sample rate, reconfiguring FIR/clocks, or reinitializing RF, it waits for two pulses again.

The on-board PPS input is PPS_EXT on J6 (the schematic connector label reads 10M_EXT), while the 10 MHz external frequency reference connects to J3. The input level must match the actual board's bank supply. OCH does not change the FPGA's LVCMOS25 constraint or the board voltages.

## LOCK display in OCH

Since 0.3.4, the reference state, PPS state, and system state all determine `locked` from the PPS frequency measurement, labeled `lock_source=PPS` and `lock_tolerance_ppm=1`.

| Monitoring situation | LOCK display |
|---|---|
| Enabled, and the most recent valid frequency is within ±1 ppm of the nominal DATA_CLK (boundary included) | `locked=1` |
| Enabled, measurement valid but outside ±1 ppm | `locked=0` |
| Off, no input, waiting for two valid pulses, invalid interval, clock unavailable, timeout, or query failed | LOCK hidden |

The decision uses an integer frequency difference, so rounding in the ppm display does not affect the boundary; it updates on every valid measurement and does not accumulate confirmation over several seconds. The SDK's check range for `frequency_valid=1` is ±10%, so it cannot be taken as lock by itself.

This is a frequency-tolerance flag relative to the input PPS, not phase lock, ADF4002 lock feedback, or a guarantee of absolute frequency accuracy. Enabling only the external 10 MHz does not need PPS, and actual ADF4002 lock still has to be checked by watching D5. Monitoring does not adjust the clock.

The CLI's normal output and the JSON `reply` hide the hardware lock/LED fields that cannot be observed; the canonical command's JSON `raw_reply` and the low-level `och_command` keep the raw SDK reply for protocol diagnostics. User interfaces should use the canonical `reply` or the C API's `lock_known/locked`, and should not show the raw SDK `locked=-1`.

## C/C++ interface

```cpp
och::device device;
device.set_pps_enabled(true);
auto status = device.pps_status();
if (status.frequency_valid) {
    // status.measured_hz and status.error_ppm are valid.
}
if (status.lock_known) {
    // Show status.locked: 1 = frequency within ±1 ppm, 0 = outside.
} else {
    // Hide LOCK.
}
device.set_pps_enabled(false);
```

C ABI: `och_set_pps_enabled(device, 0|1)`, `och_get_pps_status(device, &status)`; `och_pps_status.struct_size` must be set. An invalid frequency is represented in the C struct by `frequency_valid=0` and zero values. 0.3.4 added `lock_known/locked` in former reserved slots; `och_pps_status` is still 96 bytes and `error_ppm` is still at offset 56, preserving ABI 1. When `lock_known=0`, `locked=-1`, and the user interface must hide it. `och_reference_status.lock_known/locked` likewise represent the PPS frequency monitoring result; the reference getter downgrades an unsupported/failed optional PPS query to unknown, while the explicit PPS getter still reports the error.

The in-process API can be called while RX/TX streams are running, and also while an RX lease buffer is held. A failed query/switch does not stop streams or invalidate the RF configuration. Write operations through the raw `och_command` still require that no streams exist; applications that are streaming should use the typed interfaces above rather than opening a separate CLI to take over the device.

## Board implementation and verification

The new bitstream includes revision 2 of the PPS receive IP. The PPS interrupt is wired concat In0 → IRQ_F2P[0] → GIC 61, and the SDK uses this board mapping explicitly; the `XPAR_FABRIC_AXI_AD9361_GPS_PPS_IRQ_INTR=91` auto-generated by the current BSP does not match the actual wiring, and 91 is left for the existing CH32 interrupt. The driver shares the existing GIC, uses rising-edge triggering, and unmasks PPS on both the RX and TX sides. The CH32H417 project was not modified.

With real external 10 MHz and PPS connected, D5 stays lit; at 30.72 MSPS the stable reading is 122880000–122880001 Hz with an offset of 0–0.008 ppm. This result is relative to the input PPS and is not an absolute accuracy verification with an independent instrument. For background on the counting function, see the [ADI AXI AD9361 documentation](https://analogdevicesinc.github.io/hdl/2023_R2/library/axi_ad9361/index.html); the specific count correction and interrupt mapping follow this project's RTL/wiring.

## 40 MHz tuning mode (SDK `vcxo=1`)

The new hardware connects the CH32H417's PA4 (DAC1_OUT) to the ADF4002 charge pump output, which is the tuning node of the 40 MHz VCTCXO (CP→R109 100 kΩ→Vc, with R101/R123 1 MΩ/1 MΩ biasing it to 1.65 V); the former PA4 LED has been removed. The two drive sources cannot operate at the same time, and the mode is set by combining two switches:

| Mode | `clock.ref-source` | `pps.input-enabled` | ADF4002 | PA4 | LOCK |
|---|---|---|---|---|---|
| 0 | internal | off | CE low, charge pump high-impedance | High-impedance | Hidden; Vc held at 1.65 V by the bias resistors |
| 1 | external | off | Phase-locked to J3 10 MHz | High-impedance | Lock not read (as before, assumed locked; watch D5) |
| 2 | internal | on | CE low, charge pump high-impedance | CH32 DAC closed loop | `lock_source=CH32_PPS_LOOP` |
| 3 | external | on | Phase-locked to J3 10 MHz | High-impedance | PPS ±1 ppm frequency check, for reference only |

**Switching rules.** Turning the charge pump off takes effect immediately. When turning the charge pump on (entering 1 or 3), the SDK first asks the CH32 to release PA4, and only enables the ADF4002 after receiving the CH32's `pa4=0` reply to that request; `clock_ref_source=1` first replies `pending=1`, and OCH polls `vcxo_status?` for up to 5 s. If no reply arrives within 3 s, the charge pump stays high-impedance and `error=-116` is reported, so `och set clock.ref-source external` fails. PA4 is high-impedance after CH32 power-up and reset, and is only driven after receiving mode 2 from the Zynq.

**CH32 loop (mode 2).** On every PPS edge the Zynq uploads a line of counts, with the expected value computed exactly from the BBPLL integer and fractional words. The CH32 processes them in this order:

1. On first entry, the DAC is written to 2048, the same as the bias voltage, so the takeover is glitch-free.
2. CAL: the DAC steps by 400 LSB to measure the slope K (ppb/LSB), taking about 36 s. This step shifts the 40 MHz by a few hundred ppb to a few ppm, depending on the oscillator's tuning slope.
3. ACQ: the frequency is corrected every 8 s.
4. TRACK: a phase PI loop with τ=15 s and ζ=0.707 drives the PPS phase error to 0. The DAC is rounded with a first-order Σ-Δ at 1 Hz, carrying the rounding error into the next second, so the average frequency equals the fractional PI output and the limit cycle of jumping back and forth between two codes is avoided.
5. If no new sample arrives for more than 3 s, it enters HOLD and the DAC stays unchanged; a single jump of more than 500 ppb is treated as a glitch, and that sample is discarded and the phase reset to zero. A phase error beyond ±2 µs counts as one phase slip: only the phase is reset to zero, the frequency integrator and DAC are kept, and it continues in TRACK without falling back to ACQ.

On re-entering mode 2, ACQ starts directly from the previous DAC value and K. The loop runs locally on the CH32 and keeps working even when the host is not reading EP2. While changing the sample rate, FIR, or running `rf_reinit!`, there are no counts and the loop is in HOLD.

**Lock decision.** `locked=1` means the CH32 is in TRACK with a phase error within ±1000 ns. The measured slope is about 10.6 ppb/LSB. When streaming starts or stops, the board temperature changes and the 40 MHz drifts by about 100–140 ppb within a minute; with τ = 15 s, a simulation using the measured slope gives a phase peak of about 550 ns for this thermal transient, and about 80 ns for a slow drift of 2 ppb/min (with τ=40 s the thermal transient would exceed 2 µs). Σ-Δ is applied only at the 1 Hz loop rate, without faster dithering, to avoid modulation sidebands on the RF LO.

```powershell
och set pps.input-enabled on        # internal reference → mode 2, disciplined by the CH32
och get clock.vcxo-status
och set clock.ref-source external   # → mode 3, the CH32 releases PA4 first
```

`och_get_vcxo_status` / `device.vcxo_status()` return `och_vcxo_status`:

- `requested_mode` / `applied_mode` / `pending` / `error`: mode request and execution status.
- `pll_enabled`: whether the ADF4002 charge pump is on.
- `bridge_alive`: a CH32 reply was received within 3 s.
- `pa4_driving`, `state` (OFF/CALIBRATE/ACQUIRE/TRACK/HOLD), `dac_code`, `k_ppb_per_lsb`, `error_ppb`, `phase_ns`, `samples`, `locked`: only valid when `bridge_alive=1`; otherwise `state=UNKNOWN`, `pa4_driving=-1`, `dac_code=-1`.

In mode 2, `och_reference_status.lock_known/locked` also use this result. The PPS switch can be toggled directly while streams are running, without stopping them.
