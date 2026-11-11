"""gr-soapy source: prints rx_time / rx_rate / rx_freq tags at start, after a retune and after a
sample-rate change (SOAPY_PLAN.md 4.2). Needs one device and GNU Radio 3.10+.

    SOAPY_SDR_PLUGIN_PATH=<build>/bindings/soapy python3 bindings/soapy/hwtest/gr_rx_time.py
"""
import time
import numpy as np
import pmt
from gnuradio import gr, soapy

class TagLog(gr.sync_block):
    def __init__(self):
        gr.sync_block.__init__(self, "taglog", in_sig=[np.complex64], out_sig=None)
        self.tags = []
    def work(self, input_items, output_items):
        n = len(input_items[0])
        for t in self.get_tags_in_window(0, 0, n):
            key = pmt.symbol_to_string(t.key)
            v = t.value
            if key == "rx_time":
                v = pmt.to_uint64(pmt.tuple_ref(v, 0)) + pmt.to_double(pmt.tuple_ref(v, 1))
            else:
                v = pmt.to_python(v)
            self.tags.append((t.offset, key, v))
        return n

for nchan in (1, 2):
    tb = gr.top_block()
    src = soapy.source("driver=omnicore", "fc32", nchan, "", "", [""] * nchan, [""] * nchan)
    src.set_sample_rate(0, 10e6)
    src.set_frequency(0, 1e9)
    logs = [TagLog() for _ in range(nchan)]
    for c in range(nchan):
        tb.connect((src, c), logs[c])
    tb.start()
    time.sleep(1.5)
    src.set_frequency(0, 1.2e9)   # gr-soapy re-tags after set_frequency
    time.sleep(1.0)
    src.set_sample_rate(0, 5e6)   # time jump: plugin returns OVERFLOW, gr-soapy re-tags
    time.sleep(1.5)
    tb.stop(); tb.wait()
    for c in range(nchan):
        print(f"nchan={nchan} ch{c}: samples={logs[c].nitems_read(0)}")
        times = {}
        for off, key, val in logs[c].tags:
            times.setdefault(off, {})[key] = val
        for off in sorted(times):
            d = times[off]
            print(f"    offset={off:>10}  rx_time={d.get('rx_time', float('nan')):.7f}  rate={d.get('rx_rate')}  freq={d.get('rx_freq')}")
    # rx_time check: between consecutive tags time must advance by samples / rate of the earlier segment
    t = sorted((off, d) for off, d in times.items())
    for (o1, d1), (o2, d2) in zip(t, t[1:]):
        if "rx_time" in d1 and "rx_time" in d2:
            predicted = d1["rx_time"] + (o2 - o1) / d1["rx_rate"]
            print(f"    {o1}->{o2}: rx_time advanced {d2['rx_time'] - d1['rx_time']:.7f} s, "
                  f"samples/rate predicts {predicted - d1['rx_time']:.7f} s")
