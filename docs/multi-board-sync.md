# Multi-Board Sync

Since OCH 0.7.0, `och sync` and `och_multi_sync()` can synchronize several OmniCore SDR boards to the same frequency reference and the same sample time. Requires Zynq firmware [v2026.11.07.2](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07.2) or later (SDK_CMD_1.2, FPGA with OC_SYNC_AXI, capability field `sync=1`). For details of the hardware, FPGA, and SDK commands, see `docs/multi-board-sync.md` in OmniCore-SDR-Firmware.

## Two wiring options

**Daisy chain (default, `--mode chain`)**: no external reference needed.

```text
Master J13 REF_OUT ──> Slave1 J3 10M_EXT      Slave1 J13 ──> Slave2 J3 ...
Master J9 TRIG ──> 1-to-N splitter ──┬──> Master J1 AD9361_SYNC
                                     ├──> Slave1 J1
                                     └──> Slave2 J1   (equal cable lengths on every branch)
```

The master runs free on its own 40 MHz. If 10 MHz is connected to the master's J3, add `--master-ext-ref` and the whole chain follows it.

**Common reference (`--mode common`)**: for example a rubidium clock or GPSDO with multiple outputs.

```text
10 MHz distribution ──> J3 on every board        PPS distribution ──> J6 PPS_EXT on every board
For MCS: on each board, connect its own J9 to its own J1 with a short cable
```

## Commands

```bash
och find                                            # note the serial numbers; the master goes first
och sync --devices MASTER,SLAVE1,SLAVE2 --sample-rate 30.72M --monitor 1
och sync status --devices MASTER,SLAVE1,SLAVE2
och sync off --devices MASTER,SLAVE1,SLAVE2         # turn off REF_OUT and TRIG
```

| Option | Description |
|---|---|
| `--devices A,B,...` | Device serial numbers, master first; can be omitted when only one board is connected |
| `--mode chain\|common` | Wiring, `chain` by default |
| `--sample-rate HZ` | Configure all boards with the same sample rate before syncing (you can also run `och config` yourself first) |
| `--no-mcs` | Skip AD9361 multi-chip sync |
| `--master-ext-ref` | In a daisy chain, also lock the master to the 10 MHz on its J3 |
| `--monitor 1` | Keep a 1 s periodic TRIG after syncing, used as a common PPS to keep monitoring lock |
| `--start-time N` | Sample time loaded on the sync edge, 0 by default |
| `--loopback` | Single-board test: the master's TRIG loops back to its own SYNC counter inside the FPGA, no cable needed; must be combined with `--no-mcs` |

## Sync procedure

`och_multi_sync()` runs the following steps in order; if any step fails, it reports exactly which board and which step:

1. **Reference source**: in a daisy chain the master uses the internal reference and the slaves use external 10 MHz; with a common reference all boards use external 10 MHz. Every board enables REF_OUT; in a daisy chain only the master enables TRIG.
2. **Lock check**: in a daisy chain, the master sends a TRIG every 100 ms, and each board measures the interval between consecutive SYNC edges with its own 40 MHz; it counts as locked after 3 consecutive intervals of 4000000 ± 1. With a common reference, the PPS interval is checked to be 40000000 ± 1. This check replaces the ADF4002 lock indicator, which the CPU cannot read.
3. **AD9361 MCS**: all boards run steps 0–5 in turn; after steps 2 and 4 a SYNC pulse is sent and each board is confirmed to have received it.
4. **Load time**: all boards load `start_time` on the same TRIG edge (daisy chain) or the same PPS edge (common reference), then each board is checked for valid time with `time_last_pps == start_time`.
5. **Continuous monitoring** (`--monitor`): the master keeps a periodic TRIG. `och sync status` shows each board's SYNC interval and the sample time of the most recent edge, which should be the same on every board.

After syncing, use the existing timed interfaces to transmit and receive on several boards at once: create `OCH_RX_TIMED` streams with the same `start_time`, or use `och_send_burst` with the same `OCH_TX_HAS_TIME` time.

## C API

```c
och_device* devs[2];                         // devs[0] is the master
och_open("MASTER", &devs[0]); och_open("SLAVE1", &devs[1]);
och_config c; och_default_config(&c); c.sample_rate = 30720000;
for (int i = 0; i < 2; ++i) och_configure(devs[i], &c);

och_multi_sync_args a; och_default_multi_sync_args(&a);   // CHAIN, with MCS
a.monitor_period = 40000000;                               // 1 s periodic TRIG
och_multi_sync_result r[2] = {{sizeof r[0]}, {sizeof r[1]}};
if (och_multi_sync(devs, 2, &a, r) != OCH_OK) puts(och_last_error());
```

Single-board interfaces: `och_get_sync_status`, `och_set_sync` (REF_OUT, TRIG, pulse width, period, output edge, loopback), `och_sync_trigger` (immediately / on the next PPS / cancel), `och_sync_clear`, `och_mcs_step`. `och_set_time_source` gains `OCH_TIME_SOURCE_SYNC`. Each received SYNC edge produces an `OCH_EVENT_SYNC_IN` event, where `count` is the number of new edges that record stands for.

## Limitations

- MCS aligns the sample clocks. The phase of each board's AD9361 LO is random after every tune; you need to calibrate with a common signal or share an external LO (TXLO_EXT / RXLO_EXT).
- After changing the sample rate or reinitializing the AD9361, run `och sync` again.
- No RX/TX streams may be running during sync.
- 0.7.0 has only been tested on a single board with `--loopback --no-mcs`. The REF_OUT chain, slave lock, SYNC over cable, and the effect of MCS still need to be verified with two or more boards.
