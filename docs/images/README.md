# Documentation Images

The official images used by the README and `docs/`, kept as SVG diagrams and PNG terminal images. The SVGs in this directory are in English; `zh-CN/` holds the Chinese versions used by `docs/zh-CN/` and `README.zh-CN.md`. The PNG terminal images are shared by both languages.

| File | Contents | Data source |
|---|---|---|
| `banner-light.svg` / `banner-dark.svg` | README header banner (I/Q constellation, waveform, wordmark) for light and dark GitHub themes; Chinese subtitle versions in `zh-CN/` | `README.md` / `README.zh-CN.md` |
| `architecture.svg` | Software layers, control plane / data plane, USB link, and board components | `docs/architecture.md` |
| `rx-boundaries.svg` | RX framed block header time, overflow GAP, epochs, and metadata records | Metadata flags in `src/core/stream.cpp` |
| `timestamps.svg` | Timing of PPS alignment, timed RX, timed TX bursts, and events | `docs/timestamps.md`, FPGA `CH32H417_IP` 1.3 |
| `reconfigure.svg` | Pause, apply, and resume flow for runtime reconfiguration | `docs/architecture.md` |
| `wire-format.svg` | packed12 / signed8 byte layout, application formats, 16 KiB framed blocks | `docs/iq8.md`, codec implementation |
| `clock-tree.svg` | External 10 MHz, ADF4002, AD9361 clock chain, external LO, PPS monitoring | Device `clock status` / `adf4002_status` read-back |
| `throughput.svg` | Measured USB throughput versus theoretical wire rate | CLI hardware tests on 2026-10-25 and long 8-bit 61.44 MSPS test (OCH 0.4.2) |
| `linux-throughput.svg` | Measured USB3 throughput on Linux versus theoretical wire rate | Hardware tests on Ubuntu 24.04.3, 2026-10-25 (OCH 0.4.3) |
| `usb2-throughput.svg` | Measured throughput of the seven passing USB2 configurations versus theoretical wire rate | 300-second USB2 tests, 2026-10-25 (OCH 0.4.3) |
| `cli-usb2-info.png` | USB2 device information, same layout as USB3 | Device information from the measurement records, drawn by the CLI display code |
| `cli-usb2-bench.png` | 300-second test output, 8-bit dual-channel full duplex at 5 MSPS | 300-second test records, drawn by the CLI display code |
| `cli-*.png` | Terminal output screenshots | See below |

`cli-info.png` is a real Windows Terminal screenshot (OCH 0.7.1, Zynq `SDK_CMD_1.2_20261107_SYNC`, CH32 bridge 1.0.1, USB 5 Gbit/s, 2026-11-08). `cli-bench-running.png` and `cli-bench-summary.png` are also real screenshots (OCH 0.6.2, firmware v2026.11.07.1, USB 5 Gbit/s, 2026-11-07; 2T2R 12-bit 44 MSPS, TX attenuation 89.75 dB). The other terminal images were drawn with the CLI display code (`apps/och/presentation.hpp`) and the default Windows Terminal color scheme, using measured data from the 0.4.x period (firmware `SDK_CMD_1.0_20261024_IQ8`); omitted parts of longer output are marked with `...`.

When changing a diagram, update both the English file here and its Chinese counterpart in `zh-CN/`.
