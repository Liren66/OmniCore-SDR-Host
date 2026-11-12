"""S4 hardware acceptance: 2T2R full duplex through gr-soapy, 12-bit and 8-bit. Needs one device.

    SOAPY_SDR_PLUGIN_PATH=<build>/bindings/soapy python3 bindings/soapy/hwtest/duplex_accept.py [--cable]

Each configuration runs a GNU Radio flowgraph (two tones into a 2-channel soapy sink, a 2-channel soapy
source into a probe) and checks: 20 start/stop cycles of the whole flowgraph; a 10 s run with LO and
gain changes (rates held, rx_time continuous); a sample-rate change (new rx_rate / rx_time tags, 12-bit
only); both tones visible in RX.

Transmits only at 2.45 GHz (2.44 GHz during the retune) at TX gain 10 (79.75 dB attenuation). Without
a cable the tones reach RX through on-board leakage, so the channel mapping is only reported. With
--cable (TX1 -> RX1 and TX2 -> RX2, each through at least 30 dB attenuation) RX1 must see TX1's tone
stronger than TX2's, and RX2 the other way round.
"""
import sys
import time

import numpy as np
import pmt
from gnuradio import analog, gr, soapy

FREQ = 2.45e9
TONES = (1e6, -2e6)
results = []
cable = "--cable" in sys.argv


def report(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


class Probe(gr.sync_block):
    """Counts samples per channel, records rx_time / rx_rate tags of channel 0, keeps the last chunk."""

    def __init__(self, nch, keep=1 << 15):
        gr.sync_block.__init__(self, "probe", [np.complex64] * nch, None)
        self.nch, self.keep = nch, keep
        self.reset()

    def reset(self):
        self.n = [0] * self.nch
        self.tags = []  # (offset, key, value)
        self.last = [np.zeros(0, np.complex64)] * self.nch

    def work(self, inp, out):
        for t in self.get_tags_in_window(0, 0, len(inp[0])):
            key = pmt.symbol_to_string(t.key)
            if key == "rx_time":
                v = pmt.to_python(t.value)
                self.tags.append((t.offset, key, v[0] + v[1]))
            elif key == "rx_rate":
                self.tags.append((t.offset, key, pmt.to_double(t.value)))
        for ch in range(self.nch):
            self.n[ch] += len(inp[ch])
            if len(inp[ch]) >= 4096:
                self.last[ch] = inp[ch][-self.keep:].copy()
        return len(inp[0])


class Counter(gr.sync_block):
    def __init__(self):
        gr.sync_block.__init__(self, "counter", [np.complex64], None)
        self.n = 0

    def work(self, inp, out):
        self.n += len(inp[0])
        return len(inp[0])


class Duplex(gr.top_block):
    def __init__(self, dev_args, rate):
        gr.top_block.__init__(self, "duplex_accept")
        self.src = soapy.source("driver=omnicore", "fc32", 2, dev_args, "", [""] * 2, [""] * 2)
        self.snk = soapy.sink("driver=omnicore", "fc32", 2, dev_args, "", [""] * 2, [""] * 2, "")
        for ch in range(2):
            # The calls GRC always makes (SOAPY_PLAN.md 9.2).
            for b in (self.src, self.snk):
                b.set_sample_rate(ch, rate)
                b.set_bandwidth(ch, 0)
                b.set_frequency(ch, FREQ)
                b.set_frequency_correction(ch, 0)
                b.set_dc_offset(ch, 0)
                b.set_iq_balance(ch, 0)
            self.src.set_antenna(ch, "RX")
            self.snk.set_antenna(ch, "TX")
            self.src.set_gain_mode(ch, False)
            self.src.set_dc_offset_mode(ch, False)
            self.src.set_gain(ch, 40)
            self.snk.set_gain(ch, 10)
        self.tones = [analog.sig_source_c(rate, analog.GR_COS_WAVE, f, 0.3, 0) for f in TONES]
        self.probe = Probe(2)
        self.sent = Counter()
        for ch in range(2):
            self.connect(self.tones[ch], (self.snk, ch))
            self.connect((self.src, ch), (self.probe, ch))
        self.connect(self.tones[0], self.sent)

    def set_rate(self, rate):
        for ch in range(2):
            self.src.set_sample_rate(ch, rate)
            self.snk.set_sample_rate(ch, rate)
            self.tones[ch].set_sampling_freq(rate)

    def timed_run(self, seconds, action=None):
        self.probe.reset()
        self.sent.n = 0
        t0 = time.monotonic()
        self.start()
        if action:
            time.sleep(seconds / 2)
            action()
            time.sleep(seconds / 2)
        else:
            time.sleep(seconds)
        elapsed = time.monotonic() - t0
        self.stop()
        self.wait()
        return elapsed


def tone_db(x, rate, f):
    """Power at f over the spectrum median, dB."""
    if len(x) < 1024:
        return float("nan")
    spec = np.abs(np.fft.fft(x * np.hanning(len(x)))) ** 2
    freqs = np.fft.fftfreq(len(x), 1 / rate)
    k = int(np.argmin(np.abs(freqs - f)))
    return 10 * np.log10(spec[max(k - 3, 0):k + 4].max() / np.median(spec))


def time_breaks(tags, rate):
    """rx_time tags that do not continue the previous one at the given rate (overflows)."""
    times = [(o, v) for o, k, v in tags if k == "rx_time"]
    return sum(1 for (o1, t1), (o2, t2) in zip(times, times[1:]) if abs((t2 - t1) - (o2 - o1) / rate) > 2 / rate)


def run_config(label, dev_args, rate):
    tb = Duplex(dev_args, rate)

    # 20 start/stop cycles of the whole flowgraph (GNU Radio stops and restarts both directions).
    failures = []
    for cycle in range(20):
        try:
            el = tb.timed_run(1.0)
            low = [n for n in tb.probe.n if n < 0.7 * rate * el]
            if low or tb.sent.n < 0.7 * rate * el:
                failures.append((cycle, [round(n / el / 1e6, 2) for n in tb.probe.n], round(tb.sent.n / el / 1e6, 2)))
        except Exception as e:  # noqa: BLE001
            failures.append((cycle, str(e)))
    report(f"{label}: 20 flowgraph start/stop cycles", not failures, f"failures={failures[:3]}")

    # 10 s with LO and gain changes halfway.
    def retune():
        for ch in range(2):
            tb.src.set_gain(ch, 30)
            tb.snk.set_gain(ch, 5)
        tb.src.set_frequency(0, FREQ - 10e6)
        tb.snk.set_frequency(0, FREQ - 10e6)
        time.sleep(0.5)
        tb.src.set_frequency(0, FREQ)
        tb.snk.set_frequency(0, FREQ)

    el = tb.timed_run(10.0, retune)
    rx = [n / el for n in tb.probe.n]
    tx = tb.sent.n / el
    breaks = time_breaks(tb.probe.tags, rate)
    ntime = sum(1 for _, k, _ in tb.probe.tags if k == "rx_time")
    report(f"{label}: 10 s with LO / gain changes", min(rx) > 0.97 * rate and tx > 0.97 * rate and breaks == 0
           and ntime >= 3,
           f"RX {[round(r / 1e6, 2) for r in rx]} MSPS, TX {tx / 1e6:.2f} MSPS, rx_time tags={ntime}, breaks={breaks}")

    snr = [[tone_db(tb.probe.last[ch], rate, f) for f in TONES] for ch in range(2)]
    seen = all(max(s) > 20 for s in snr)
    mapped = snr[0][0] > snr[0][1] and snr[1][1] > snr[1][0]
    report(f"{label}: TX tones in RX", seen and (mapped or not cable),
           f"RX1 (+1 MHz, -2 MHz) = ({snr[0][0]:.1f}, {snr[0][1]:.1f}) dB, RX2 = ({snr[1][0]:.1f}, {snr[1][1]:.1f}) dB, "
           f"channel mapping {'as wired' if mapped else 'not separated'}{'' if cable else ' (no cable: informational)'}")
    return tb


def main():
    tb = run_config("12-bit 2T2R 30.72 MSPS", "rx_mask=3,tx_mask=3", 30.72e6)

    # Sample-rate change while running: a new rx_rate tag and a fresh rx_time reference.
    new_rate = 15.36e6
    tb.set_rate(30.72e6)
    el = tb.timed_run(4.0, lambda: tb.set_rate(new_rate))
    rates = [v for _, k, v in tb.probe.tags if k == "rx_rate"]
    ntime = sum(1 for _, k, _ in tb.probe.tags if k == "rx_time")
    report("12-bit: sample-rate change while running", new_rate in rates and ntime >= 2,
           f"rx_rate tags={rates}, rx_time tags={ntime}")
    del tb

    run_config("8-bit 2T2R 61.44 MSPS", "rx_mask=3,tx_mask=3,iq_bits=8", 61.44e6)

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed", "; failed: " + ", ".join(failed) if failed else "")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
