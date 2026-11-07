#!/usr/bin/env python3
"""Throughput sweep: highest error-free sample rate per channel configuration.

python tools/bench_sweep.py <och.exe> <out.json> [--modes 0,1] [--seconds 5]
Each run: och bench ... --json; pass = stability.pass and zero host drops,
transfer errors, device overflows/underflows, event drops and TX starvation.
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

def run(och, args, rate, framing, seconds):
    cmd = [och, "bench", *args, "--sample-rate", str(rate), "--seconds", str(seconds),
           "--attenuation", "89.75", "--json"]
    if framing is not None:
        cmd += ["--framing", str(framing)]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds * 6 + 60)
    result = None
    for line in p.stdout.splitlines():
        if line.startswith('{"command":"bench"'):
            result = json.loads(line)
    if result is None:
        return {"ok": False, "error": (p.stdout + p.stderr)[-400:]}
    bad = 0
    out = {"ok": True}
    for d in ("rx", "tx"):
        s = result.get(d)
        if not s:
            continue
        out[d + "_MBps"] = round(s["usb_MBps"], 2)
        bad += s["host_dropped_samples"] + s["transfer_errors"] + s["device_overflows"] + \
               s["device_underflows"] + s["event_drops"] + s["tx_starvation_events"]
    out["errors"] = bad
    out["pass"] = bool(result["stability"]["pass"]) and bad == 0
    return out

def main():
    och, out_path = sys.argv[1], sys.argv[2]
    modes = [1]
    seconds = 5
    if "--modes" in sys.argv:
        modes = [None if m == "x" else int(m) for m in sys.argv[sys.argv.index("--modes") + 1].split(",")]
    if "--seconds" in sys.argv:
        seconds = int(sys.argv[sys.argv.index("--seconds") + 1])
    results = []
    for name, args in CONFIGS:
        for mode in modes:
            best = None
            tried = []
            for rate in RATES:
                r = run(och, args, rate, mode, seconds)
                tried.append({"rate": rate, **r})
                print(f"{name:11s} framing={mode} {rate/1e6:6.2f} MS/s -> {r}", flush=True)
                if r.get("pass"):
                    best = {"rate": rate, **r}
                    break
            results.append({"config": name, "framing": mode, "best": best, "tried": tried})
            json.dump(results, open(out_path, "w"), indent=1)
    print("SWEEP_DONE")

if __name__ == "__main__":
    main()
