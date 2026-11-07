#!/usr/bin/env python3
"""Throughput sweep: highest error-free sample rate per channel configuration.

python tools/bench_sweep.py <och.exe> <out.json> [--seconds 5] [--configs 1RX,2RX]

Each run is `och bench ... --json` (TX sends zero IQ at 89.75 dB attenuation).
A run passes when stability.pass is true and host drops, USB transfer errors,
device overflows/underflows, event drops and TX starvation are all zero.
Rates are tried from the top; the first passing rate is reported.
"""
import json, subprocess, sys

CONFIGS = [
    ("1RX",        ["--rx", "--rx-channels", "1"]),
    ("2RX",        ["--rx", "--rx-channels", "1,2"]),
    ("1T1R",       ["--rx", "--tx", "--rx-channels", "1", "--tx-channels", "1"]),
    ("2T1R",       ["--rx", "--tx", "--rx-channels", "1", "--tx-channels", "1,2"]),
    ("1T2R",       ["--rx", "--tx", "--rx-channels", "1,2", "--tx-channels", "1"]),
    ("2T2R 12bit", ["--rx", "--tx", "--rx-channels", "1,2", "--tx-channels", "1,2"]),
    ("2T2R 8bit",  ["--rx", "--tx", "--rx-channels", "1,2", "--tx-channels", "1,2", "--iq-bits", "8"]),
]
RATES = [61440000, 56000000, 50000000, 46000000, 42000000, 38000000, 34000000, 30720000, 25000000]
COUNTERS = ("host_dropped_samples", "transfer_errors", "device_overflows", "device_underflows",
            "event_drops", "tx_starvation_events")


def run(och, args, rate, seconds):
    cmd = [och, "bench", *args, "--sample-rate", str(rate), "--seconds", str(seconds), "--json"]
    if "--tx" in args:
        cmd += ["--amplitude", "0", "--attenuation", "89.75"]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds * 6 + 60)
    result = None
    for line in p.stdout.splitlines():
        if line.startswith('{"command":"bench"'):
            result = json.loads(line)
    if result is None:
        return {"ok": False, "error": (p.stdout + p.stderr)[-400:]}
    out = {"ok": True}
    bad = 0
    for d in ("rx", "tx"):
        s = result.get(d)
        if not s:
            continue
        out[d + "_MBps"] = round(s["usb_MBps"], 2)
        bad += sum(s[k] for k in COUNTERS)
    out["errors"] = bad
    out["pass"] = bool(result["stability"]["pass"]) and bad == 0
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    och, out_path = sys.argv[1], sys.argv[2]
    seconds = int(sys.argv[sys.argv.index("--seconds") + 1]) if "--seconds" in sys.argv else 5
    wanted = sys.argv[sys.argv.index("--configs") + 1].split(",") if "--configs" in sys.argv else None
    results = []
    for name, args in CONFIGS:
        if wanted and name not in wanted:
            continue
        best, tried = None, []
        for rate in RATES:
            r = run(och, args, rate, seconds)
            tried.append({"rate": rate, **r})
            print(f"{name:11s} {rate/1e6:6.2f} MS/s -> {r}", flush=True)
            if r.get("pass"):
                best = {"rate": rate, **r}
                break
        results.append({"config": name, "best": best, "tried": tried})
        json.dump(results, open(out_path, "w"), indent=1)
    print("SWEEP_DONE")


if __name__ == "__main__":
    main()
