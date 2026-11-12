"""S2 hardware acceptance for the OmniCore SoapySDR module (RX only). Needs one device.

    SOAPY_SDR_PLUGIN_PATH=<build>/bindings/soapy python3 bindings/soapy/hwtest/rx_accept.py
"""
import gc
import sys
import time

import numpy as np
import SoapySDR
from SoapySDR import SOAPY_SDR_RX, SOAPY_SDR_CS16, SOAPY_SDR_CF32
from SoapySDR import SOAPY_SDR_HAS_TIME, SOAPY_SDR_END_BURST
from SoapySDR import SOAPY_SDR_TIMEOUT, SOAPY_SDR_OVERFLOW, SOAPY_SDR_TIME_ERROR

results = []


def report(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


class Reader:
    """Reads a stream and checks that time advances by exactly the samples delivered."""

    def __init__(self, sdr, stream, nch, fmt):
        self.sdr, self.stream, self.nch = sdr, stream, nch
        mtu = sdr.getStreamMTU(stream)
        dtype = np.complex64 if fmt == SOAPY_SDR_CF32 else np.int16
        width = mtu if fmt == SOAPY_SDR_CF32 else 2 * mtu
        self.bufs = [np.zeros(width, dtype) for _ in range(nch)]
        self.mtu = mtu
        self.reset()

    def reset(self):
        self.samples = self.overflows = self.timeouts = self.time_errors = self.errors = 0
        self.no_time = self.time_jumps = self.end_bursts = 0
        self.expected_ns = None
        self.first_ns = None
        self.last_flags = 0

    def read(self, rate, timeout_us=500000):
        sr = self.sdr.readStream(self.stream, self.bufs, self.mtu, timeoutUs=timeout_us)
        if sr.ret == SOAPY_SDR_TIMEOUT:
            self.timeouts += 1
        elif sr.ret == SOAPY_SDR_OVERFLOW:
            self.overflows += 1
            self.expected_ns = None  # the next block starts a new time reference
        elif sr.ret == SOAPY_SDR_TIME_ERROR:
            self.time_errors += 1
        elif sr.ret < 0:
            self.errors += 1
        else:
            if not sr.flags & SOAPY_SDR_HAS_TIME:
                self.no_time += 1
            if self.first_ns is None:
                self.first_ns = sr.timeNs
            if self.expected_ns is not None and abs(sr.timeNs - self.expected_ns) > 1e9 / rate:
                self.time_jumps += 1
            self.expected_ns = sr.timeNs + sr.ret * 1e9 / rate
            self.samples += sr.ret
            if sr.flags & SOAPY_SDR_END_BURST:
                self.end_bursts += 1
            self.last_flags = sr.flags
        return sr

    def run(self, rate, seconds):
        end = time.monotonic() + seconds
        t0 = time.monotonic()
        while time.monotonic() < end:
            self.read(rate)
        return self.samples / (time.monotonic() - t0)

    def summary(self):
        return (f"samples={self.samples} overflow={self.overflows} timeout={self.timeouts} "
                f"time_err={self.time_errors} err={self.errors} no_time={self.no_time} jumps={self.time_jumps}")


def open_dev(**extra):
    # A plain dict fails in the SoapySDR 0.8 Python binding ("make() no match"); a string works.
    args = ",".join(["driver=omnicore"] + [f"{k}={v}" for k, v in extra.items()])
    return SoapySDR.Device(args)


def throughput(sdr, channels, fmt, rate, seconds):
    for ch in channels:
        sdr.setSampleRate(SOAPY_SDR_RX, ch, rate)
    rx = sdr.setupStream(SOAPY_SDR_RX, fmt, channels)
    r = Reader(sdr, rx, len(channels), fmt)
    sdr.activateStream(rx)
    # Drop the first second: startup overflow is allowed (docs/usb2.md test conditions).
    r.run(rate, 1.0)
    r.reset()
    got = r.run(rate, seconds)
    power = [float(np.mean(np.abs(b.astype(np.complex64 if fmt == SOAPY_SDR_CF32 else np.float32)) ** 2)) for b in r.bufs]
    sdr.deactivateStream(rx)
    sdr.closeStream(rx)
    ok = r.overflows == 0 and r.errors == 0 and r.no_time == 0 and r.time_jumps == 0 and got > 0.98 * rate
    report(f"throughput {len(channels)}ch {fmt} {rate/1e6:.2f} MSPS {seconds}s", ok,
           f"{got/1e6:.3f} MSPS/ch, {r.summary()}, mean power per ch={['%.3g' % p for p in power]}")


def main():
    sdr = open_dev()
    sdr.setGainMode(SOAPY_SDR_RX, 0, False)
    for ch in (0, 1):
        sdr.setGain(SOAPY_SDR_RX, ch, 30)
    sdr.setFrequency(SOAPY_SDR_RX, 0, 1e9)

    throughput(sdr, [0], SOAPY_SDR_CS16, 30.72e6, 10)
    throughput(sdr, [0], SOAPY_SDR_CF32, 61.44e6, 10)
    throughput(sdr, [1], SOAPY_SDR_CS16, 30.72e6, 5)
    throughput(sdr, [0, 1], SOAPY_SDR_CS16, 61.44e6, 10)
    throughput(sdr, [0, 1], SOAPY_SDR_CF32, 61.44e6, 30)

    # LO / gain change while streaming: no overflow, time continuous.
    rate = 30.72e6
    sdr.setSampleRate(SOAPY_SDR_RX, 0, rate)
    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CF32, [0, 1])
    r = Reader(sdr, rx, 2, SOAPY_SDR_CF32)
    sdr.activateStream(rx)
    r.run(rate, 1.0)
    r.reset()
    for i, f in enumerate((1.1e9, 2.4e9, 900e6, 1e9)):
        sdr.setFrequency(SOAPY_SDR_RX, 0, f)
        sdr.setGain(SOAPY_SDR_RX, i % 2, 20 + 10 * i)
        r.run(rate, 0.5)
    report("retune LO and gain while streaming", r.overflows == 0 and r.errors == 0 and r.time_jumps == 0,
           r.summary() + f", freq now {sdr.getFrequency(SOAPY_SDR_RX, 0)/1e6:.1f} MHz")

    # RX gain limits follow the LO band (73 / 71 / 62 dB). Too-high requests are clamped, and a retune
    # lowers a gain the new band cannot take; the device must never reject one (that stops streams).
    r.reset()
    seen = []
    try:
        sdr.setFrequency(SOAPY_SDR_RX, 0, 2.45e9)
        sdr.setGain(SOAPY_SDR_RX, 0, 76)
        seen.append(("2450 MHz set 76", sdr.getGain(SOAPY_SDR_RX, 0), sdr.getGainRange(SOAPY_SDR_RX, 0).maximum()))
        r.run(rate, 0.3)
        sdr.setFrequency(SOAPY_SDR_RX, 0, 1e9)
        sdr.setGain(SOAPY_SDR_RX, 0, 76)
        seen.append(("1000 MHz set 76", sdr.getGain(SOAPY_SDR_RX, 0), sdr.getGainRange(SOAPY_SDR_RX, 0).maximum()))
        r.run(rate, 0.3)
        for f in (2.45e9, 5.8e9):
            sdr.setFrequency(SOAPY_SDR_RX, 0, f)
            seen.append((f"retune {f/1e6:.0f} MHz", sdr.getGain(SOAPY_SDR_RX, 0),
                         sdr.getGainRange(SOAPY_SDR_RX, 0).maximum()))
            r.run(rate, 0.3)
        error = None
    except Exception as e:  # noqa: BLE001
        error = str(e)
    want = [71, 73, 71, 62]
    report("RX gain limited by LO band while streaming",
           error is None and [g for _, g, _ in seen] == want and [m for _, _, m in seen] == want
           and r.overflows == 0 and r.errors == 0 and r.samples > 0,
           f"(step, gain, max)={seen}, error={error}, " + r.summary())
    sdr.setFrequency(SOAPY_SDR_RX, 0, 1e9)
    sdr.setGain(SOAPY_SDR_RX, 0, 30)

    # Sample-rate change while streaming: expect OVERFLOW, then data with a new time reference.
    r.reset()
    new_rate = 20e6
    sdr.setSampleRate(SOAPY_SDR_RX, 0, new_rate)
    applied = sdr.getSampleRate(SOAPY_SDR_RX, 0)
    r.run(applied, 2.0)
    got_rate = r.samples / 2.0
    report("sample-rate change while streaming", r.overflows >= 1 and r.errors == 0 and r.time_jumps == 0,
           r.summary() + f", applied={applied:.0f} Hz, delivered≈{got_rate/1e6:.2f} MSPS")
    sdr.deactivateStream(rx)
    sdr.closeStream(rx)

    # Repeated activate/deactivate on one stream, and setup/close cycles.
    rate = 30.72e6
    sdr.setSampleRate(SOAPY_SDR_RX, 0, rate)
    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
    r = Reader(sdr, rx, 1, SOAPY_SDR_CS16)
    failures = 0
    t0 = time.monotonic()
    for _ in range(20):
        try:
            sdr.activateStream(rx)
            r.reset()
            r.run(rate, 0.2)
            if r.samples == 0 or r.errors:
                failures += 1
            if sdr.deactivateStream(rx) != 0:
                failures += 1
        except Exception as e:  # noqa: BLE001
            failures += 1
            print("   cycle error:", e)
    sdr.closeStream(rx)
    report("20 activate/deactivate cycles", failures == 0, f"failures={failures}, {time.monotonic()-t0:.1f}s total")

    failures = 0
    for chans in ([0], [0, 1], [1], [0]):
        try:
            rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, chans)
            r = Reader(sdr, rx, len(chans), SOAPY_SDR_CS16)
            sdr.activateStream(rx)
            r.run(rate, 0.3)
            failures += r.samples == 0
            sdr.deactivateStream(rx)
            sdr.closeStream(rx)
        except Exception as e:  # noqa: BLE001
            failures += 1
            print("   setup error:", e)
    report("setup/close with channel changes 0 -> 0,1 -> 1 -> 0", failures == 0, f"failures={failures}")

    # Timed start: hardware time 0 now, start 0.5 s later.
    sdr.setHardwareTime(0)
    t_now = sdr.getHardwareTime()
    start_ns = t_now + 500_000_000
    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
    r = Reader(sdr, rx, 1, SOAPY_SDR_CS16)
    sdr.activateStream(rx, SOAPY_SDR_HAS_TIME, start_ns)
    deadline = time.monotonic() + 3
    while r.samples == 0 and time.monotonic() < deadline:
        r.read(rate)
    err_ns = (r.first_ns - start_ns) if r.first_ns is not None else None
    report("timed RX start at now+0.5 s", err_ns is not None and abs(err_ns) <= 1e9 / rate,
           f"requested={start_ns} ns, first sample={r.first_ns} ns, error={err_ns} ns, "
           f"time_valid={sdr.readSensor('time_valid')}")
    sdr.deactivateStream(rx)

    # Finite burst: exactly numElems samples, END_BURST on the last read.
    n = 1_000_000
    r.reset()
    sdr.activateStream(rx, SOAPY_SDR_END_BURST, 0, n)
    deadline = time.monotonic() + 3
    while r.samples < n and time.monotonic() < deadline and not r.end_bursts:
        r.read(rate, 200000)
    extra = 0
    for _ in range(3):  # anything after END_BURST would be wrong
        sr = r.read(rate, 100000)
        if sr.ret > 0:
            extra += sr.ret
    report(f"finite RX burst of {n}", r.samples == n and r.end_bursts == 1 and extra == 0,
           f"received={r.samples - extra}+{extra} after, end_bursts={r.end_bursts}, {r.summary()}")
    sdr.deactivateStream(rx)
    sdr.closeStream(rx)

    # Close with a stream left active: the destructor must clean up, and the device must reopen.
    rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
    sdr.activateStream(rx)
    del r, rx, sdr  # Reader holds the device too; drop every reference so it is destroyed
    gc.collect()
    try:
        sdr = open_dev()
        rx = sdr.setupStream(SOAPY_SDR_RX, SOAPY_SDR_CS16, [0])
        r = Reader(sdr, rx, 1, SOAPY_SDR_CS16)
        sdr.activateStream(rx)
        r.run(rate, 0.3)
        sdr.deactivateStream(rx)
        sdr.closeStream(rx)
        report("close device with an active stream, then reopen", r.samples > 0, r.summary())
    except Exception as e:  # noqa: BLE001
        report("close device with an active stream, then reopen", False, str(e))

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed", "; failed: " + ", ".join(failed) if failed else "")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
