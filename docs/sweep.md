# Sweep Waterfall Example

[`examples/sweep.cpp`](../examples/sweep.cpp) steps the RX LO on a continuously running RX stream, stitches the FFT of each step into a full spectrum (one waterfall row per sweep), and records how long each retune takes. [`examples/sweep_plot.py`](../examples/sweep_plot.py) does the plotting.

```bash
sweep --start 70M --stop 6G --sweeps 20 --out scan      # 20 sweeps over the full range
python3 sweep_plot.py scan                              # spectrum + waterfall + timing histogram + retune power trace
python3 sweep_plot.py scan --follow                     # run alongside sweep and refresh live (use with --sweeps 0)
```

## Demo UI

[`examples/sweep_live.py`](../examples/sweep_live.py) starts `sweep --stream` and serves a local web page ([`sweep_live.html`](../examples/sweep_live.html)), using only the Python standard library:

```bash
python3 examples/sweep_live.py --sweep build/examples/sweep --open   # opens http://127.0.0.1:8765
```

- **Top cards**: time per sweep, sweep speed (GHz/s, steps/s), median same-band retune time (also converted to a theoretical maximum retunes/s), retunes that cross gain tables, total time per step, and data integrity (RX discontinuities, dropped samples, re-captures).
- **Spectrum**: live trace and max hold (orange); a light-blue band marks the segment the LO is currently sweeping, and the dashed lines are the 1.3 / 4 GHz gain-table boundaries. Hover to read frequency and level.
- **Waterfall**: the last 150 sweeps, with automatic color scaling that can also be adjusted by hand.
- **Interaction**: pick a range with the presets (full range, FM, cellular, 2.4G, 5G WiFi, L band), or drag a box on the spectrum or waterfall to zoom in, and double-click to return to the full range. Clicking an entry in the "Strongest signals" list zooms to ±20 MHz around that signal. Changes to sample rate, gain, kept fraction per segment, and averaging count take effect after pressing "Apply", which restarts sweep.
- When the whole range fits in a single step, no retuning is needed and it behaves like a fixed-tuned real-time spectrum analyzer: the server limits refresh to 25 frames per second, and the speed card shows "Fixed tuning".
- Whenever a data discontinuity occurs, the log records the sweep, the step, and the LO frequency where it happened.
- The device stays held by sweep while the demo runs. On Ctrl+C (or SIGTERM), sweep stops the stream cleanly and closes the device.

Measured: about 1.0 s per full-range sweep (161 steps of about 5.2 ms each, `--no-bracket`); 2.39–2.5 GHz takes only 3 steps, about 17 ms per sweep, fast enough to see WiFi bursts directly.

Defaults: 61.44 MSPS, 2048-point FFT (30 kHz resolution) × 16 averages, 64 KiB USB transfers (`--transfer-kib`), the middle 60% of FFT bins kept per step (36.84 MHz step, 161 steps for the full range), manual gain 30 dB, and the DC leakage at the LO center smoothed by interpolating from 5 bins on each side. `sweep --help` lists all options. When the range is not a whole number of steps, the grid is shifted back to fit within 70 MHz–6 GHz.

## Determining which samples belong to the new frequency

Fast tuning does not stop the stream, so the `RECONFIGURED` flag on the next block is only an approximate boundary: when `och_configure` returns, the USB queue may still hold a whole queue of old data (library default 32 × 192 KiB, about 68 ms at 30.72 MSPS single-channel). The example uses hardware time to determine the boundary instead:

1. Before retuning, read the device time once as `before` (`och_get_time`, i.e. `time_status?`);
2. `och_configure` changes only `rx_frequency`, taking the fast tuning path;
3. After it returns, read the device time again as `after`. The SDK processes commands in order, so the LO write and the PLL lock wait are guaranteed to have happened before `after`;
4. Only use samples whose timestamp is no earlier than `after + --guard-us` (default 50 µs, covering the AD9361 digital filter delay), regardless of the USB queue depth.

`after - before` is an upper bound on the retune interval measured in sample time, and includes two command round trips. `--no-bracket` skips the first time read, saving about 1.8 ms per step, but then there is no interval or power trace.

## Output files

| File | Contents |
|---|---|
| `PREFIX.json` | Frequency axis (`first_bin_hz`, `bin_hz`, `row_bins`) and parameters; `summary` is written at the end |
| `PREFIX.waterfall.f32` | One row of float32 dBFS per sweep, row length `row_bins` |
| `PREFIX.steps.jsonl` | One line per step: `configure_us` (host-side retune time), `time_reads_us`, `capture_us`, `total_us`, `bracket_us`, `gain_table`, `discarded`, `restarts` |
| `PREFIX.trace.jsonl` | Power trace for each step in the first `--trace-sweeps` sweeps: starting at `before`, one point every 64 samples |

`--stream` writes no files and instead prints JSON line by line on standard output: one `meta` line, one `step` line per step, one `row` line per sweep (where `db` is base64-encoded little-endian float32 data), and a `summary` line at the end. Human-readable messages go to standard error.

dBFS takes a full-scale complex sine as 0 dBFS; it is not calibrated input power.

## Measured results (2026-11-14)

macOS, USB 3.0, Zynq `SDK_CMD_1.2_20261107_SYNC`, CH32 bridge 1.0.1; 70 MHz–6 GHz, single channel. All values are medians over same-band steps.

Transfer size and time reads (30.72 MSPS, 258 steps × 23.04 MHz):

| Configuration | Retune | Time reads | Capture | Per step | Per sweep | RX discontinuities |
|---|---:|---:|---:|---:|---:|---:|
| 192 KiB transfers (library default), two time reads | 3.04 ms | 3.55 ms | 3.36 ms | 10.0 ms | 2.76 s | 0 (20 sweeps) |
| 64 KiB, two time reads | 3.03 ms | 3.53 ms | 0.47 ms | 7.1 ms | — | 0 (5 sweeps) |
| 16 KiB, two time reads | 3.03 ms | 3.52 ms | 0.25 ms | 6.8 ms | — | **1 overflow** (5 sweeps) |
| 64 KiB, `--no-bracket` | 3.02 ms | 1.76 ms | — | 5.3 ms | about 1.45 s | 0 (10 sweeps) |

Sample rate (all with 64 KiB transfers, 0 RX discontinuities):

| Sample rate / FFT / kept fraction | Step | Steps | Per step | Per sweep | Resolution per bin |
|---|---:|---:|---:|---:|---:|
| 30.72 MSPS / 1024 / 0.75, `--no-bracket` | 23.04 MHz | 258 | 5.36 ms | 1.55 s | 30 kHz |
| 61.44 MSPS / 1024 / 0.75, `--no-bracket` | 46.08 MHz | 129 | 5.20 ms | 0.78 s | 60 kHz |
| 61.44 MSPS / 2048 / 0.6, `--no-bracket` | 36.84 MHz | 161 | 5.23 ms | 1.00 s | 30 kHz |
| **61.44 MSPS / 2048 / 0.6 (current default, two time reads)** | 36.84 MHz | 161 | 7.12 ms | 1.31 s (20 sweeps) | 30 kHz |

Time per step barely changes with sample rate: it is dominated by the retune, and capture takes only about 0.5 ms. So doubling the sample rate halves the number of steps and roughly halves the time per sweep. The cost is single-channel USB traffic rising from 92 MB/s to 184 MB/s, still far below the measured limit. Keeping the resolution per bin the same (FFT increased to 2048) keeps the capture time per step the same.

- The median for steps crossing gain tables is 29.6 ms; there are about 4 per sweep, adding up to about 0.12 s per sweep.
- Samples only reach the host once a whole USB transfer is full: 192 KiB at 30.72 MSPS is about 2.1 ms. That is why the example defaults to 64 KiB. 16 KiB has already produced a device overflow and is not recommended.
- One `time_status?` takes about 1.76 ms, much slower than `rx_lo_freq?` (about 0.6 ms), and is the largest cost apart from the retune itself.
- The noise floor at both edges of each segment is higher than in the middle, by up to about 7.6 dB in the 1.3–4 GHz range. Narrowing the analog bandwidth from 27.6 MHz to 20 MHz barely helps (+7.1 dB), so it is not aliasing of signals outside the analog passband. It is more likely noise folding back near Nyquist after the ADC (245.76 MHz) goes through three half-band decimation stages. Comparison (median of 3 sweeps):

  | Settings | Step | Per sweep | Edge rise (<1.3G / 1.3–4G / >4G) | Center residue |
  |---|---:|---:|---|---|
  | `--usable 0.75 --dc-bins 2` (old default) | 23.04 MHz | 1.55 s | +1.8 / +7.6 / +4.8 dB | up to +1.7 dB |
  | `--usable 0.6 --dc-bins 5` | 18.42 MHz | 1.91 s | +0.2 / +2.1 / +0.9 dB | up to +0.9 dB |

  At 61.44 MSPS the edges behave the same: +0.9 / +6.2 / +4.3 dB with 0.75 kept, +0.6 / +1.9 / +0.9 dB with 0.6, and +1.9 / +8.9 / +6.6 dB with 0.85. The example therefore defaults to 61.44 MSPS with `--usable 0.6` and `--dc-bins 5`, which is both faster than the old 30.72 MSPS with 0.75 and cleaner at the edges. A complete fix needs a real low-pass FIR combined with ×2 decimation (ADC at 491.52 MHz). Note: the FIR only acts before decimation, so a ×1 FIR cannot remove noise that has already folded in.

## Notes on reading the results

- Steps with `gain_table=1` cross 1.3 GHz / 4 GHz (or fall within ±1 MHz of them), so the SDK rewrites the gain table, which takes about 25 ms; the first step of each sweep, jumping from 6 GHz back to the start, also falls into this category. The histogram lists them separately.
- Time per step = retune + two time reads + capture (default 2048 × 16 = 32768 samples ≈ 0.53 ms) + FFT. The current per-step bottleneck is the firmware executing `rx_lo_freq=` (the AD9361 driver recalibrates the VCO and waits for lock on every retune). Tens of thousands of hops per second would need firmware support for AD9361 fastlock and device-side timed hopping, which the current firmware does not yet provide.
- A non-zero `restarts` means a time discontinuity (overflow or dropped samples) occurred while capturing a step, and that step was captured again from the start; `rx_gaps`, `host_dropped_samples`, and `device_overflows` in `summary` should be 0.
- In wideband sweeps, the RX DC / IQ tracking loops do not have time to converge at each step and a center spike may remain; adjust the smoothing width with `--dc-bins`.
