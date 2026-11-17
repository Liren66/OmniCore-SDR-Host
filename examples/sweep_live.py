#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live sweep demo: runs `sweep --stream` and serves a browser view.

    python3 sweep_live.py --sweep PATH/TO/sweep            # then open http://127.0.0.1:8765
    python3 sweep_live.py --sweep build/examples/sweep --open

Python standard library only. The page (sweep_live.html, next to this file)
shows the spectrum, a scrolling waterfall and retune timing, and can change the
range, sample rate and gain; each change restarts sweep. Only one sweep runs,
and it holds the device while the demo is up.
"""
import argparse
import array
import base64
import json
import math
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time
import webbrowser
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
DISPLAY_BINS = 4096  # rows are max-pooled to this many points: peaks survive
HISTORY_ROWS = 400
DEFAULTS = {"start": 70e6, "stop": 6e9, "rate": 61.44e6, "gain": 30.0, "usable": 0.6, "average": 16}
LIMITS = {"start": (70e6, 6e9), "stop": (70e6, 6e9), "rate": (2.5e6, 61.44e6), "gain": (0.0, 73.0),
          "usable": (0.3, 0.9), "average": (1, 64)}


def downsample(row, n):
    """Max-pool a row to at most n points."""
    if len(row) <= n:
        return list(row)
    out, step = [], len(row) / n
    for i in range(n):
        a, b = int(i * step), int((i + 1) * step)
        out.append(max(row[a:b]))
    return out


def median(v):
    if not v:
        return None
    v = sorted(v)
    return v[len(v) // 2]


class Hub:
    """Owns the sweep process and fans its output out to browser clients."""

    def __init__(self, sweep, serial):
        self.sweep, self.serial = sweep, serial
        self.lock = threading.Lock()
        self.clients = []
        self.proc = None
        self.generation = 0
        self.config = dict(DEFAULTS)
        self.reset_state()

    def reset_state(self):
        self.meta = None
        self.rows = deque(maxlen=HISTORY_ROWS)
        self.steps = deque(maxlen=4000)
        self.stats = None
        self.status = {"type": "status", "state": "starting", "message": ""}
        self.last_progress = 0.0
        self.last_row = 0.0
        self.faults = (0, 0, 0)  # rx_gaps, host_dropped, restarts so far

    # --- clients ---------------------------------------------------------
    def subscribe(self):
        q = queue.Queue(maxsize=2000)
        with self.lock:
            self.clients.append(q)
            snapshot = [self.status, {"type": "config", **self.config}]
            if self.meta:
                snapshot.append(self.meta)
            snapshot += list(self.rows)
            if self.stats:
                snapshot.append(self.stats)
        for item in snapshot:
            q.put(item)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.clients:
                self.clients.remove(q)

    def broadcast(self, item):
        with self.lock:
            clients = list(self.clients)
        for q in clients:
            try:
                q.put_nowait(item)
            except queue.Full:  # a stalled tab loses updates, not the server
                pass

    def set_status(self, state, message=""):
        self.status = {"type": "status", "state": state, "message": message}
        self.broadcast(self.status)

    # --- process ---------------------------------------------------------
    def start(self, config=None):
        self.stop()
        if config:
            self.config = config
        c = self.config
        args = [self.sweep, "--stream", "--sweeps", "0", "--no-bracket",
                "--start", str(int(c["start"])), "--stop", str(int(c["stop"])), "--rate", str(int(c["rate"])),
                "--gain", f"{c['gain']:g}", "--usable", f"{c['usable']:g}", "--average", str(int(c["average"]))]
        if self.serial:
            args += ["--serial", self.serial]
        with self.lock:
            self.generation += 1
            gen = self.generation
            self.reset_state()
        self.broadcast({"type": "reset"})
        self.broadcast({"type": "config", **c})
        self.set_status("starting", " ".join(os.path.basename(a) if i == 0 else a for i, a in enumerate(args)))
        try:
            self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, bufsize=1)
        except OSError as e:
            self.set_status("error", f"cannot start {self.sweep}: {e}")
            return
        threading.Thread(target=self.read_stdout, args=(self.proc, gen), daemon=True).start()
        threading.Thread(target=self.read_stderr, args=(self.proc, gen), daemon=True).start()

    def stop(self):
        p, self.proc = self.proc, None
        if not p or p.poll() is not None:
            return
        # sweep finishes its step, stops the stream and closes the device on SIGINT.
        # Windows cannot deliver SIGINT to a child without a console group: terminate it.
        if os.name == "nt":
            p.terminate()
        else:
            p.send_signal(signal.SIGINT)
        try:
            p.wait(timeout=4)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
        self.set_status("stopped")

    def read_stderr(self, proc, gen):
        for line in proc.stderr:
            line = line.rstrip()
            if line and gen == self.generation:
                self.broadcast({"type": "log", "line": line})

    def read_stdout(self, proc, gen):
        for line in proc.stdout:
            if gen != self.generation:
                break
            try:
                item = json.loads(line)
            except ValueError:
                continue
            kind = item.get("type")
            if kind == "meta":
                n = min(item["row_bins"], DISPLAY_BINS)
                last = item["first_bin_hz"] + item["bin_hz"] * (item["row_bins"] - 1)
                item.update(display_bins=n, display_first_hz=item["first_bin_hz"], display_last_hz=last)
                self.meta = item
                self.broadcast(item)
                self.set_status("running")
            elif kind == "step":
                self.steps.append(item)
                faults = (item["rx_gaps"], item["host_dropped"], self.faults[2] + item["restarts"])
                if faults != self.faults:  # say exactly when and where data went missing
                    self.broadcast({"type": "log", "line": f"Data gap: sweep {item['sweep'] + 1}, step {item['step'] + 1}, "
                                    f"LO {item['frequency'] / 1e6:.2f} MHz; total RX gaps {faults[0]}, "
                                    f"dropped {faults[1]}, re-captures {faults[2]}"})
                    self.faults = faults
                now = time.monotonic()
                if now - self.last_progress > 0.05:
                    self.last_progress = now
                    self.broadcast({"type": "progress", "sweep": item["sweep"], "step": item["step"],
                                    "frequency": item["frequency"]})
            elif kind == "row":
                self.on_row(item)
            elif kind == "summary":
                self.broadcast(item)
        code = proc.wait()
        if gen == self.generation:
            if code == 0 or code == -signal.SIGINT:
                self.set_status("stopped")
            else:
                self.set_status("error", f"sweep exited with code {code}")

    def on_row(self, item):
        # A range that fits in one step needs no retune and yields a row every
        # millisecond or so: keep the browser at a readable 25 rows/s.
        now = time.monotonic()
        if now - self.last_row < 0.04:
            return
        self.last_row = now
        raw = array.array("f")
        raw.frombytes(base64.b64decode(item["db"]))
        if sys.byteorder != "little":
            raw.byteswap()
        row = {"type": "row", "sweep": item["sweep"], "seconds": item["seconds"],
               "db": [round(v, 1) for v in downsample(raw, DISPLAY_BINS)]}
        self.rows.append(row)
        self.broadcast(row)

        sweep = item["sweep"]
        mine = [s for s in self.steps if s["sweep"] == sweep and not (s["sweep"] == 0 and s["step"] == 0)]
        recent = [s for s in self.steps if s["sweep"] >= sweep - 4 and not (s["sweep"] == 0 and s["step"] == 0)]
        same = [s["configure_us"] for s in mine if not s["gain_table"]]
        table = [s["configure_us"] for s in mine if s["gain_table"]]
        total = [s["total_us"] for s in mine]
        last = self.steps[-1] if self.steps else {}
        steps = self.meta["steps"] if self.meta else len(mine)
        # Retune-time histogram over the last few sweeps, log-spaced 0.5..100 ms.
        edges = [0.5 * (200 ** (i / 40)) for i in range(41)]
        counts = [0] * 40
        for s in recent:
            ms = s["configure_us"] / 1e3
            if ms >= edges[0]:
                counts[min(39, int(40 * math.log(ms / 0.5) / math.log(200)))] += 1
        self.stats = {
            "type": "stats", "sweep": sweep, "sweeps": sweep + 1, "seconds": item["seconds"],
            "steps": steps, "steps_per_second": steps / item["seconds"] if item["seconds"] else 0,
            "ghz_per_second": (self.meta["row_bins"] * self.meta["bin_hz"] / 1e9 / item["seconds"])
            if self.meta and item["seconds"] else 0,
            "retune_ms": (median(same) or 0) / 1e3, "retune_gain_table_ms": (median(table) or 0) / 1e3,
            "gain_table_steps": len(table), "step_ms": (median(total) or 0) / 1e3,
            "capture_ms": (median([s["capture_us"] for s in mine]) or 0) / 1e3,
            "rx_gaps": last.get("rx_gaps", 0), "host_dropped": last.get("host_dropped", 0),
            "restarts": sum(s["restarts"] for s in self.steps),
            "hist_edges_ms": edges, "hist_counts": counts,
        }
        self.broadcast(self.stats)


def validate(body):
    c = dict(DEFAULTS)
    for key, (lo, hi) in LIMITS.items():
        if key in body:
            v = float(body[key])
            if not (lo <= v <= hi) or math.isnan(v):
                raise ValueError(f"{key} must be within {lo:g}..{hi:g}")
            c[key] = v
    if c["stop"] <= c["start"]:
        raise ValueError("stop must be above start")
    return c


def make_handler(hub):
    page = os.path.join(HERE, "sweep_live.html")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):  # keep the terminal quiet
            pass

        def send(self, code, body, kind="application/json"):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                with open(page, "rb") as f:
                    self.send(200, f.read(), "text/html; charset=utf-8")
            elif self.path == "/events":
                self.events()
            else:
                self.send(404, '{"error":"not found"}')

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
                if self.path == "/config":
                    hub.start(validate(body))
                elif self.path == "/stop":
                    hub.stop()
                elif self.path == "/start":
                    hub.start()
                else:
                    return self.send(404, '{"error":"not found"}')
                self.send(200, '{"ok":true}')
            except ValueError as e:
                self.send(400, json.dumps({"error": str(e)}))

        def events(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            q = hub.subscribe()
            try:
                while True:
                    try:
                        item = q.get(timeout=15)
                        self.wfile.write(b"data: " + json.dumps(item, separators=(",", ":")).encode() + b"\n\n")
                    except queue.Empty:
                        self.wfile.write(b": keep-alive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                hub.unsubscribe(q)
                self.close_connection = True

    return Handler


def find_sweep(given):
    candidates = [given] if given else []
    candidates += [shutil.which("sweep"), os.path.join(HERE, "sweep"), os.path.join(HERE, "sweep.exe")]
    for c in candidates:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    sys.exit("sweep executable not found; pass --sweep PATH (built as examples/sweep)")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--sweep", help="path to the sweep example executable")
    p.add_argument("--serial", help="device serial")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--open", action="store_true", help="open the page in the default browser")
    a = p.parse_args()

    # Ctrl+C and SIGTERM both shut down cleanly, so sweep closes the device. A
    # shell's background job starts with SIGINT ignored: install it explicitly.
    def interrupt(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, interrupt)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, interrupt)

    hub = Hub(find_sweep(a.sweep), a.serial)
    server = ThreadingHTTPServer((a.host, a.port), make_handler(hub))
    server.daemon_threads = True
    url = f"http://{a.host}:{a.port}/"
    print(f"OmniCore sweep demo on {url}  (Ctrl+C to quit)", flush=True)
    hub.start()
    if a.open:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        hub.stop()
        server.server_close()


if __name__ == "__main__":
    main()
