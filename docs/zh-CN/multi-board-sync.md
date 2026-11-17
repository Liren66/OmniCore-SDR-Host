# 多板同步

OCH 0.7.0 起，`och sync` 和 `och_multi_sync()` 可以把多块 OmniCore SDR 同步到同一个频率参考和同一个样点时间。需要 Zynq 固件 [v2026.11.07.2](https://github.com/Liren66/OmniCore-SDR-Firmware/releases/tag/v2026.11.07.2) 及以上（SDK_CMD_1.2，FPGA 带 OC_SYNC_AXI，能力字段 `sync=1`）。硬件、FPGA 与 SDK 命令的细节见 OmniCore-SDR-Firmware 的 `docs/multi-board-sync.md`。

## 两种接法

**菊花链（默认，`--mode chain`）**：不需要外部参考。

```text
主板 J13 REF_OUT ──> 从板1 J3 10M_EXT      从板1 J13 ──> 从板2 J3 ……
主板 J9 TRIG ──> 1 分 N ──┬──> 主板 J1 AD9361_SYNC
                          ├──> 从板1 J1
                          └──> 从板2 J1        （各分支线长相同）
```

主板用自己的 40 MHz 自由运行。如果主板 J3 接了 10 MHz，加 `--master-ext-ref`，整条链都会跟随它。

**公共参考（`--mode common`）**：例如带多路输出的铷钟或 GPSDO。

```text
10 MHz 分配 ──> 每块板 J3        PPS 分配 ──> 每块板 J6 PPS_EXT
做 MCS 时：每块板用一根短线把自己的 J9 接到自己的 J1
```

## 命令

```bash
och find                                            # 记下序列号，主板放第一个
och sync --devices MASTER,SLAVE1,SLAVE2 --sample-rate 30.72M --monitor 1
och sync status --devices MASTER,SLAVE1,SLAVE2
och sync off --devices MASTER,SLAVE1,SLAVE2         # 关闭 REF_OUT 和 TRIG
```

| 选项 | 说明 |
|---|---|
| `--devices A,B,...` | 设备序列号，第一个是主板；只连了一块板时可以省略 |
| `--mode chain\|common` | 接法，默认 `chain` |
| `--sample-rate HZ` | 同步前用同一采样率配置所有板（也可以先自己 `och config`） |
| `--no-mcs` | 跳过 AD9361 多片同步 |
| `--master-ext-ref` | 菊花链时主板也锁到它 J3 上的 10 MHz |
| `--monitor 1` | 同步后保留 1 s 周期的 TRIG，作为公共 PPS 持续监视锁定 |
| `--start-time N` | 同步沿上装载的样点时间，默认 0 |
| `--loopback` | 单板测试：主板 TRIG 在 FPGA 内部回到自己的 SYNC 计数器，不需要线缆；必须同时加 `--no-mcs` |

## 同步过程

`och_multi_sync()` 依次执行以下步骤，任何一步失败都会报出具体是哪块板、哪一步：

1. **参考源**：菊花链时主板用内部参考，从板用外部 10 MHz；公共参考时所有板都用外部 10 MHz。每块板打开 REF_OUT，菊花链时只有主板打开 TRIG。
2. **锁定检查**：菊花链时，主板每 100 ms 发一次 TRIG，每块板用自己的 40 MHz 测量相邻 SYNC 沿的间隔，连续 3 次等于 4000000 ± 1 才算锁定。公共参考时检查 PPS 间隔等于 40000000 ± 1。这个检查替代了 CPU 读不到的 ADF4002 锁定指示。
3. **AD9361 MCS**：所有板依次执行第 0–5 步，第 2、4 步之后发一个 SYNC 脉冲，并确认每块板都收到了。
4. **装载时间**：所有板在同一个 TRIG 沿（菊花链）或同一个 PPS 沿（公共参考）装载 `start_time`，然后确认每块板的时间有效，并且 `time_last_pps == start_time`。
5. **持续监视**（`--monitor`）：主板保留周期 TRIG。`och sync status` 显示每块板的 SYNC 间隔和最近一个沿的样点时间，各板应当相同。

同步完成后，用现有的定时接口在多块板上同时收发：同一个 `start_time` 创建 `OCH_RX_TIMED` 流，或者用 `och_send_burst` 带同一个 `OCH_TX_HAS_TIME` 时间。

## C API

```c
och_device* devs[2];                         // devs[0] 是主板
och_open("MASTER", &devs[0]); och_open("SLAVE1", &devs[1]);
och_config c; och_default_config(&c); c.sample_rate = 30720000;
for (int i = 0; i < 2; ++i) och_configure(devs[i], &c);

och_multi_sync_args a; och_default_multi_sync_args(&a);   // CHAIN，带 MCS
a.monitor_period = 40000000;                               // 1 s 周期 TRIG
och_multi_sync_result r[2] = {{sizeof r[0]}, {sizeof r[1]}};
if (och_multi_sync(devs, 2, &a, r) != OCH_OK) puts(och_last_error());
```

单板接口：`och_get_sync_status`、`och_set_sync`（REF_OUT、TRIG、脉宽、周期、发出边沿、回环）、`och_sync_trigger`（立即 / 在下一个 PPS / 取消）、`och_sync_clear`、`och_mcs_step`。`och_set_time_source` 增加 `OCH_TIME_SOURCE_SYNC`。收到 SYNC 沿时产生 `OCH_EVENT_SYNC_IN` 事件，`count` 是这一行代表的新沿数。

## 限制

- MCS 对齐的是采样时钟。各板 AD9361 本振的相位每次调谐都是随机的，需要用公共信号校准，或者共享外部本振（TXLO_EXT / RXLO_EXT）。
- 改采样率或重新初始化 AD9361 后，要重新执行 `och sync`。
- 同步期间不能有正在运行的 RX/TX 流。
- 0.7.0 只在单板上用 `--loopback --no-mcs` 实测过。REF_OUT 链路、从板锁定、线缆上的 SYNC 和 MCS 效果，还需要两块以上的板子验证。
