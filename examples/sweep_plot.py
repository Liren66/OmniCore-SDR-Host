#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plot the output of the sweep example: spectrum, waterfall, retune timing.

    python3 sweep_plot.py PREFIX              # plot once
    python3 sweep_plot.py PREFIX --follow     # redraw while sweep is running
    python3 sweep_plot.py PREFIX --save scan.png

Needs numpy and matplotlib. PREFIX is the --out of sweep (default "sweep").
"""
import argparse
import json
import os
import sys

import numpy as np


def read_jsonl(path):
    rows = []
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line.endswith("}"):  # skip a line still being written
                    rows.append(json.loads(line))
    return rows


def load(prefix):
    with open(prefix + ".json") as f:
        meta = json.load(f)
    n = meta["row_bins"]
    path = os.path.join(os.path.dirname(prefix), os.path.basename(meta["waterfall"]))
    raw = np.fromfile(path, dtype=np.float32) if os.path.exists(path) else np.zeros(0, np.float32)
    rows = raw[: raw.size // n * n].reshape(-1, n)
    freq = meta["first_bin_hz"] + meta["bin_hz"] * np.arange(n)
    return meta, freq, rows, read_jsonl(prefix + ".steps.jsonl"), read_jsonl(prefix + ".trace.jsonl")


def pick_traces(traces, count=4):
    """A few retunes worth looking at: the slowest, a gain-table one, typical ones."""
    if not traces:
        return []
    by_len = sorted(traces, key=lambda t: t["after_us"])
    picks = [by_len[len(by_len) // 2], by_len[-1]]
    for t in by_len[: len(by_len) // 2 : max(1, len(by_len) // 8)]:
        if len(picks) >= count:
            break
        picks.append(t)
    seen, out = set(), []
    for t in picks:
        if t["step"] not in seen:
            seen.add(t["step"])
            out.append(t)
    return out


def draw(fig, axes, prefix, vmin, vmax):
    meta, freq, rows, steps, traces = load(prefix)
    ghz = freq / 1e9
    for ax in axes.values():
        ax.clear()

    ax = axes["spectrum"]
    if len(rows):
        ax.plot(ghz, rows[-1], lw=0.6)
        ax.set_ylim(vmin, max(vmax, float(np.max(rows[-1])) + 3))
    ax.set_xlim(ghz[0], ghz[-1])
    ax.set_ylabel("dBFS")
    ax.set_title(
        f"{prefix}: {meta['steps']} steps x {meta['step_hz'] / 1e6:.2f} MHz, "
        f"{meta['sample_rate'] / 1e6:.2f} MSPS, gain {meta['gain_db']:g} dB, sweeps {len(rows)}"
    )
    ax.grid(alpha=0.3)

    ax = axes["waterfall"]
    if len(rows):
        ax.imshow(rows[::-1], aspect="auto", origin="lower", cmap="viridis", vmin=vmin, vmax=vmax,
                  extent=(ghz[0], ghz[-1], 0, len(rows)), interpolation="nearest")
    ax.set_xlabel("frequency (GHz)")
    ax.set_ylabel("sweeps ago")

    ax = axes["timing"]
    retunes = [s for s in steps if not (s["sweep"] == 0 and s["step"] == 0)]
    same = np.array([s["configure_us"] for s in retunes if not s["gain_table"]]) / 1e3
    table = np.array([s["configure_us"] for s in retunes if s["gain_table"]]) / 1e3
    total = np.array([s["total_us"] for s in retunes]) / 1e3
    if len(total):
        # Log axis: same-band retunes (a few ms) and gain-table ones (tens of ms) both stay readable.
        parts = [v for v in (same, table, total) if len(v)]
        lo = max(min(float(v.min()) for v in parts) * 0.8, 1e-2)
        hi = max(float(v.max()) for v in parts) * 1.2
        bins = np.geomspace(lo, hi, 80)
        ax.set_xscale("log")
        ax.hist(same, bins=bins, alpha=0.8, label=f"retune, same band (median {np.median(same) if len(same) else 0:.2f} ms)")
        if len(table):
            ax.hist(table, bins=bins, alpha=0.8, label=f"retune, gain table (median {np.median(table):.2f} ms)")
        ax.hist(total, bins=bins, histtype="step", color="k", label=f"whole step (median {np.median(total):.2f} ms)")
        ax.legend(fontsize=8)
        rate = 1e3 / np.median(total)
        ax.set_title(f"per-step time: {rate:.0f} steps/s, {rate * meta['step_hz'] / 1e9:.2f} GHz/s")
    ax.set_xlabel("ms")
    ax.set_ylabel("steps")

    ax = axes["trace"]
    for t in pick_traces(traces):
        us = np.arange(len(t["power_db"])) * t["bin_samples"] / t["sample_rate"] * 1e6
        line, = ax.plot(us / 1e3, t["power_db"], lw=0.7, label=f"{t['frequency'] / 1e6:.0f} MHz")
        ax.axvline(t["after_us"] / 1e3, color=line.get_color(), ls="--", lw=0.8)
    if traces:
        ax.legend(fontsize=8, title="dashed: retune done", title_fontsize=8)
        ax.set_title("RX power around a retune (0 = time read before it)")
    else:
        ax.set_title("no traces (--no-bracket or --trace-sweeps 0)")
    ax.set_xlabel("ms")
    ax.set_ylabel("dBFS")
    ax.grid(alpha=0.3)

    if meta.get("summary"):
        s = meta["summary"]
        fig.suptitle(
            f"{s['sweeps']} sweeps, median {s['sweep_seconds_median'] * 1e3:.0f} ms per sweep, "
            f"{s['steps_per_second']:.0f} steps/s; RX gaps {s['rx_gaps']}, "
            f"host dropped {s['host_dropped_samples']}, device overflows {s['device_overflows']}",
            fontsize=10)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("prefix", nargs="?", default="sweep")
    p.add_argument("--follow", action="store_true", help="redraw every second while sweep runs")
    p.add_argument("--save", metavar="PNG", help="write an image instead of opening a window")
    p.add_argument("--vmin", type=float, default=-110, help="waterfall floor, dBFS")
    p.add_argument("--vmax", type=float, default=-40, help="waterfall ceiling, dBFS")
    a = p.parse_args()
    if a.save:
        import matplotlib
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    if not os.path.exists(a.prefix + ".json"):
        sys.exit(f"{a.prefix}.json not found; run sweep --out {a.prefix} first")
    fig = plt.figure(figsize=(14, 9), constrained_layout=True)
    grid = fig.add_gridspec(3, 2, height_ratios=[1, 2, 1.2])
    axes = {
        "spectrum": fig.add_subplot(grid[0, :]),
        "waterfall": fig.add_subplot(grid[1, :]),
        "timing": fig.add_subplot(grid[2, 0]),
        "trace": fig.add_subplot(grid[2, 1]),
    }
    axes["waterfall"].sharex(axes["spectrum"])
    draw(fig, axes, a.prefix, a.vmin, a.vmax)
    if a.save:
        fig.savefig(a.save, dpi=120)
        return
    if a.follow:
        plt.ion()
        plt.show()
        while plt.fignum_exists(fig.number):
            plt.pause(1.0)
            draw(fig, axes, a.prefix, a.vmin, a.vmax)
            fig.canvas.draw_idle()
    else:
        plt.show()


if __name__ == "__main__":
    main()
