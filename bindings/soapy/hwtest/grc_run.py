"""Runs the examples/soapy flowgraphs headless and checks what their Soapy source delivers.

    grcc -o <dir> examples/soapy/*.grc
    cd <dir> && QT_QPA_PLATFORM=offscreen SOAPY_SDR_PLUGIN_PATH=<build>/bindings/soapy \
        python3 <repo>/bindings/soapy/hwtest/grc_run.py omnicore_rx_spectrum
    (same with omnicore_duplex_loopback)

rx_spectrum: 10 s at 10 MSPS, retunes to 2.40 GHz and changes gain halfway; needs rx_time tags at
start and after the retune. duplex_loopback: three start/stop cycles of the flowgraph (TX gain 20,
raised to 30 in the first cycle); needs the 1 MHz TX tone in RX more than 20 dB over the median.
Transmits only at 2.45 GHz.
"""
import sys, time, importlib
import numpy as np
from PyQt5 import Qt
from gnuradio import gr

class Probe(gr.sync_block):
    def __init__(self, keep):
        gr.sync_block.__init__(self, "probe", [np.complex64], None)
        self.keep = keep; self.reset()
    def reset(self):
        self.n = 0; self.tags = []; self.buf = np.zeros(self.keep, np.complex64); self.fill = 0
    def work(self, inp, out):
        x = inp[0]
        for t in self.get_tags_in_window(0, 0, len(x)):
            self.tags.append((t.offset, str(t.key)))
        m = min(len(x), self.keep)
        self.buf = np.roll(self.buf, -m); self.buf[-m:] = x[-m:]
        self.fill = min(self.keep, self.fill + m)
        self.n += len(x)
        return len(x)

def tone_db(x, rate, f):
    X = np.abs(np.fft.fftshift(np.fft.fft(x * np.hanning(len(x))))) ** 2
    fr = np.fft.fftshift(np.fft.fftfreq(len(x), 1 / rate))
    k = np.argmin(np.abs(fr - f))
    return 10 * np.log10(X[k-3:k+4].max() / np.median(X))

app = Qt.QApplication(sys.argv)
name = sys.argv[1]
sys.path.insert(0, ".")
mod = importlib.import_module(name)
tb = getattr(mod, name)()
rate = tb.samp_rate
probe = Probe(1 << 18)
tb.connect(tb.soapy_custom_source_0, probe)

def run(seconds, action=None):
    probe.reset(); t0 = time.monotonic(); tb.start()
    time.sleep(seconds / 2)
    if action: action()
    time.sleep(seconds / 2)
    el = time.monotonic() - t0
    tb.stop(); tb.wait()
    keys = {}
    for _, k in probe.tags: keys[k] = keys.get(k, 0) + 1
    x = probe.buf[-probe.fill:]
    return el, probe.n, keys, x

fails = 0
if name == "omnicore_rx_spectrum":
    def retune():
        tb.set_freq(2.40e9); tb.set_gain(40)
    el, n, keys, x = run(10, retune)
    ok = n > 0.95 * rate * el and keys.get("rx_time", 0) >= 2
    print(f"[{'PASS' if ok else 'FAIL'}] rx_spectrum 10 s with retune 1 GHz -> 2.40 GHz: {n/el/1e6:.2f} MSPS delivered, tags={keys}, freq now {tb.soapy_custom_source_0.get_frequency(0)/1e6:.1f} MHz")
    fails += not ok
else:
    for cycle in range(3):
        act = (lambda: tb.set_tx_gain(30)) if cycle == 0 else None
        el, n, keys, x = run(6, act)
        snr = tone_db(x, rate, 1e6)
        img = tone_db(x, rate, -1e6)
        ok = n > 0.95 * rate * el and snr > 20
        print(f"[{'PASS' if ok else 'FAIL'}] duplex cycle {cycle+1}: RX {n/el/1e6:.2f} MSPS, tone +1 MHz {snr:.1f} dB over median (image -1 MHz {img:.1f} dB), tags={keys}")
        fails += not ok
print("done, failures =", fails)
