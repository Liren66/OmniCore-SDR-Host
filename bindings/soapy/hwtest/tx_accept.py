"""S3 hardware acceptance for the OmniCore SoapySDR module (TX and full duplex). Needs one device.

    SOAPY_SDR_PLUGIN_PATH=<build>/bindings/soapy python3 bindings/soapy/hwtest/tx_accept.py

Transmits only at 2.45 GHz (ISM) and at very low power: TX gain 0 (89.75 dB attenuation) except
the leakage check, which uses gain 30 (59.75 dB attenuation). No cable or antenna is needed; the
leakage check reports what the board's own RX sees and is informational.
"""
import gc
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_TX, SOAPY_SDR_CS16, SOAPY_SDR_CF32
from SoapySDR import SOAPY_SDR_HAS_TIME, SOAPY_SDR_END_BURST
from SoapySDR import SOAPY_SDR_TIMEOUT, SOAPY_SDR_OVERFLOW, SOAPY_SDR_UNDERFLOW, SOAPY_SDR_TIME_ERROR

FREQ = 2.45e9
results = []


def report(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def open_dev(**extra):
    # A plain dict fails in the SoapySDR 0.8 Python binding ("make() no match"); a string works.
    return SoapySDR.Device(",".join(["driver=omnicore"] + [f"{k}={v}" for k, v in extra.items()]))


def tone(n, fmt, rate, freq=1e6, amp=0.3):
    x = amp * np.exp(2j * np.pi * freq / rate * np.arange(n)).astype(np.complex64)
    if fmt == SOAPY_SDR_CF32:
        return x
    out = np.empty(2 * n, np.int16)
    out[0::2] = np.round(x.real * 32767)
    out[1::2] = np.round(x.imag * 32767)
    return out


def drain_status(sdr, tx, timeout_us=0):
    """All pending TX status reports as (ret, flags, timeNs)."""
    out = []
    while True:
        sr = sdr.readStreamStatus(tx, timeoutUs=timeout_us)
        if sr.ret == SOAPY_SDR_TIMEOUT:
            return out
        out.append((sr.ret, sr.flags, sr.timeNs))


def count(statuses, code):
    return sum(1 for r, _, _ in statuses if r == code)


def acks(statuses):
    return [t for r, f, t in statuses if r == 0 and f & SOAPY_SDR_END_BURST]


def write_all(sdr, tx, bufs, n, flags=0, time_ns=0, timeout_us=1000000):
    """writeStream until all n samples are accepted; HAS_TIME only on the first call."""
    done = 0
    while done < n:
        views = [b[done * (1 if b.dtype == np.complex64 else 2):] for b in bufs]
        f = flags if done == 0 else flags & ~SOAPY_SDR_HAS_TIME
        sr = sdr.writeStream(tx, views, n - done, f, time_ns, timeoutUs=timeout_us)
        if sr.ret < 0:
            return done, sr.ret
        done += sr.ret
    return done, 0


def continuous(sdr, channels, fmt, rate, seconds):
    for ch in channels:
        sdr.setSampleRate(SOAPY_SDR_TX, ch, rate)
        sdr.setGain(SOAPY_SDR_TX, ch, 0)
    sdr.setFrequency(SOAPY_SDR_TX, 0, FREQ)
    tx = sdr.setupStream(SOAPY_SDR_TX, fmt, channels)
    mtu = sdr.getStreamMTU(tx)
    bufs = [tone(mtu, fmt, rate, 1e6 * (c + 1)) for c in range(len(channels))]
    sdr.activateStream(tx)
    sent = errors = 0
    statuses = []
    t0 = time.monotonic()
    warm = t0 + 1.0
    while time.monotonic() - t0 < seconds + 1:
        sr = sdr.writeStream(tx, bufs, mtu, timeoutUs=1000000)
        if sr.ret < 0 and sr.ret != SOAPY_SDR_TIMEOUT:
            errors += 1
        elif sr.ret > 0 and time.monotonic() > warm:
            sent += sr.ret
        st = drain_status(sdr, tx)
        if time.monotonic() > warm:
            statuses += st
    sdr.writeStream(tx, bufs, 0, SOAPY_SDR_END_BURST)
    sdr.deactivateStream(tx)
    sdr.closeStream(tx)
    got = sent / seconds
    under = count(statuses, SOAPY_SDR_UNDERFLOW)
    report(f"continuous TX {len(channels)}ch {fmt} {rate/1e6:.2f} MSPS {seconds}s",
           errors == 0 and under == 0 and got > 0.98 * rate,
           f"{got/1e6:.3f} MSPS/ch, underflow={under}, stream errors={errors}, other status={len(statuses) - under}")


def bursts(sdr):
    rate = 10e6
    sdr.setSampleRate(SOAPY_SDR_TX, 0, rate)
    sdr.setGain(SOAPY_SDR_TX, 0, 0)
    sdr.setFrequency(SOAPY_SDR_TX, 0, FREQ)
    tx = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0])
    n = 20000  # 2 ms, more than one 16 KiB block
    buf = [tone(n, SOAPY_SDR_CF32, rate)]

    # Timed bursts, 50 ms apart.
    sdr.activateStream(tx)
    sdr.setHardwareTime(0)
    base = sdr.getHardwareTime() + 200_000_000
    requested = [base + i * 50_000_000 for i in range(10)]
    write_errors = 0
    for t in requested:
        done, err = write_all(sdr, tx, buf, n, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST, t)
        write_errors += err != 0 or done != n
    time.sleep(0.8)
    st = drain_status(sdr, tx, 100000)
    got = acks(st)
    deltas = [(a - r) / 1e3 for a, r in zip(got, requested)]
    report("10 timed bursts", write_errors == 0 and len(got) == 10 and count(st, SOAPY_SDR_TIME_ERROR) == 0
           and count(st, SOAPY_SDR_UNDERFLOW) == 0,
           f"acks={len(got)}, write errors={write_errors}, statuses={[(r, f) for r, f, _ in st if r]}, "
           f"ack - request (us)={['%.1f' % d for d in deltas]}")

    # A burst whose time has passed.
    late = sdr.getHardwareTime() - 100_000_000
    write_all(sdr, tx, buf, n, SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST, late)
    st = drain_status(sdr, tx, 500000)
    report("late timed burst reports TIME_ERROR", count(st, SOAPY_SDR_TIME_ERROR) >= 1,
           f"statuses={[(r, f) for r, f, _ in st]}")

    # Untimed burst closed by a zero-length END_BURST write.
    write_all(sdr, tx, buf, n)
    sr = sdr.writeStream(tx, buf, 0, SOAPY_SDR_END_BURST)
    st = drain_status(sdr, tx, 500000)
    report("zero-length END_BURST closes the burst", sr.ret == 0 and len(acks(st)) == 1,
           f"write ret={sr.ret}, statuses={[(r, f) for r, f, _ in st]}")
    sdr.deactivateStream(tx)

    # Repeated activation, one burst each.
    failures = 0
    for _ in range(10):
        sdr.activateStream(tx)
        done, err = write_all(sdr, tx, buf, n, SOAPY_SDR_END_BURST)
        st = drain_status(sdr, tx, 500000)
        failures += err != 0 or len(acks(st)) != 1
        sdr.deactivateStream(tx)
    report("10 TX activate/burst/deactivate cycles", failures == 0, f"failures={failures}")

    # First burst shorter than one block (gr-soapy length-tag bursts). The EOB block reaches the
    # FPGA padded, so och_start's prefill gate is met (SOAPY_PLAN.md 9.3 item 1, checked 2026-11-12).
    short_failures = []
    for n_short, timed in ((1, False), (1000, False), (1000, True)):
        sdr.activateStream(tx)
        f = SOAPY_SDR_END_BURST | (SOAPY_SDR_HAS_TIME if timed else 0)
        t = sdr.getHardwareTime() + 50_000_000
        t0 = time.monotonic()
        done, err = write_all(sdr, tx, [tone(n_short, SOAPY_SDR_CF32, rate)], n_short, f, t)
        took = time.monotonic() - t0
        st = drain_status(sdr, tx, 500000)
        if err != 0 or len(acks(st)) != 1 or took > 1:
            short_failures.append((n_short, timed, err, round(took, 2), len(acks(st))))
        sdr.deactivateStream(tx)
    report("short first bursts (1 and 1000 samples, timed and untimed)", not short_failures,
           f"failures (n, timed, err, seconds, acks)={short_failures}")
    sdr.closeStream(tx)


def duplex():
    rate = 30.72e6
    sdr = open_dev(rx_mask=1, tx_mask=1)
    sdr.setSampleRate(SOAPY_SDR_RX, 0, rate)
    sdr.setFrequency(SOAPY_SDR_RX, 0, FREQ)
    sdr.setFrequency(SOAPY_SDR_TX, 0, FREQ)
    sdr.setGain(SOAPY_SDR_TX, 0, 0)
    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
    tx = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CS16, [0])
    rmtu, tmtu = sdr.getStreamMTU(rx), sdr.getStreamMTU(tx)
    rbuf = [np.zeros(2 * rmtu, np.int16)]
    tbuf = [tone(tmtu, SOAPY_SDR_CS16, rate)]

    def run(seconds):
        stats = dict(rx=0, tx=0, overflow=0, underflow=0, errors=0)
        t0 = time.monotonic()
        while time.monotonic() - t0 < seconds:
            w = sdr.writeStream(tx, tbuf, tmtu, timeoutUs=200000)
            stats["tx"] += max(w.ret, 0)
            stats["errors"] += w.ret < 0 and w.ret != SOAPY_SDR_TIMEOUT
            # Keep RX drained: one TX write is one MTU, read until RX catches up.
            for _ in range(4):
                r = sdr.readStream(rx, rbuf, rmtu, timeoutUs=0)
                if r.ret == SOAPY_SDR_TIMEOUT:
                    break
                if r.ret == SOAPY_SDR_OVERFLOW:
                    stats["overflow"] += 1
                elif r.ret < 0:
                    stats["errors"] += 1
                else:
                    stats["rx"] += r.ret
            stats["underflow"] += count(drain_status(sdr, tx), SOAPY_SDR_UNDERFLOW)
        return stats

    # Start both (GNU Radio order: RX block first, then TX), run, stop both; repeat.
    failures = 0
    first = None
    for cycle in range(5):
        try:
            sdr.activateStream(rx)
            sdr.activateStream(tx)
            st = run(10 if cycle == 0 else 1)
            if cycle == 0:
                first = st
            failures += st["errors"] > 0 or st["rx"] == 0 or st["tx"] == 0
            sdr.writeStream(tx, tbuf, 0, SOAPY_SDR_END_BURST)
            sdr.deactivateStream(tx)
            sdr.deactivateStream(rx)
        except Exception as e:  # noqa: BLE001
            failures += 1
            print("   duplex cycle error:", e)
    report("full duplex 1R1T 30.72 MSPS, 10 s then 4 restarts",
           failures == 0 and first is not None and first["underflow"] == 0,
           f"first run: RX {first['rx']/10/1e6:.2f} MSPS, TX {first['tx']/10/1e6:.2f} MSPS "
           f"(accepted writes, includes the host buffer prefill), "
           f"overflow={first['overflow']}, underflow={first['underflow']}, errors={first['errors']}; "
           f"cycle failures={failures}" if first else f"cycle failures={failures}")
    sdr.closeStream(tx)
    sdr.closeStream(rx)
    return sdr


def leakage(sdr):
    """Informational: a timed TX burst seen by the same board's RX through on-board leakage."""
    rate = 10e6
    for d in (SOAPY_SDR_RX, SOAPY_SDR_TX):
        sdr.setSampleRate(d, 0, rate)
        sdr.setFrequency(d, 0, FREQ)
    sdr.setGainMode(SOAPY_SDR_RX, 0, False)
    sdr.setGain(SOAPY_SDR_RX, 0, 60)
    sdr.setGain(SOAPY_SDR_TX, 0, 30)
    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0])
    tx = sdr.setupStream(SOAPY_SDR_TX, SOAPY_SDR_CF32, [0])
    sdr.setHardwareTime(0)
    start = sdr.getHardwareTime() + 100_000_000
    burst_at = start + 100_000_000
    n_burst = 50000
    sdr.activateStream(rx, SOAPY_SDR_HAS_TIME, start, int(0.3 * rate))
    sdr.activateStream(tx)
    write_all(sdr, tx, [tone(n_burst, SOAPY_SDR_CF32, rate, 1e6, 0.7)], n_burst,
              SOAPY_SDR_HAS_TIME | SOAPY_SDR_END_BURST, burst_at)
    mtu = sdr.getStreamMTU(rx)
    buf = np.zeros(mtu, np.complex64)
    chunks, t_first = [], None
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        r = sdr.readStream(rx, [buf], mtu, timeoutUs=200000)
        if r.ret > 0:
            if t_first is None:
                t_first = r.timeNs
            chunks.append(buf[:r.ret].copy())
            if r.flags & SOAPY_SDR_END_BURST:
                break
    sdr.deactivateStream(tx)
    sdr.deactivateStream(rx)
    sdr.closeStream(tx)
    sdr.closeStream(rx)
    if not chunks:
        print("[INFO] leakage check: no RX samples", flush=True)
        return
    # 2.45 GHz WiFi/Bluetooth is far stronger than on-board leakage, so a wideband power edge finds
    # the wrong thing. Correlate with the 1 MHz tone over the burst length instead.
    x = np.concatenate(chunks)
    y = x * np.exp(-2j * np.pi * 1e6 / rate * np.arange(len(x)))
    cs = np.concatenate([[0], np.cumsum(y)])
    c = np.abs(cs[n_burst:] - cs[:-n_burst])
    k = int(np.argmax(c))
    snr = 20 * np.log10(c[k] / np.median(c))
    expected = (burst_at - t_first) * rate / 1e9
    detail = (f"tone correlation {snr:.1f} dB over median; "
              + (f"burst at sample {k}, expected {expected:.0f} (error {(k - expected)/rate*1e6:.1f} us)"
                 if snr > 30 else "burst not visible (no RF path between TX and RX)"))
    print(f"[INFO] leakage check: {detail}", flush=True)


def main():
    sdr = open_dev()
    continuous(sdr, [0], SOAPY_SDR_CS16, 30.72e6, 10)
    continuous(sdr, [0], SOAPY_SDR_CF32, 61.44e6, 10)
    continuous(sdr, [0, 1], SOAPY_SDR_CS16, 30.72e6, 10)
    bursts(sdr)
    del sdr
    gc.collect()
    sdr = duplex()
    leakage(sdr)
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed", "; failed: " + ", ".join(failed) if failed else "")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
