// SPDX-License-Identifier: GPL-3.0-or-later
// Wideband RX sweep. One RX stream keeps running while the RX LO steps across
// the band; each step's FFT segment is stitched into one waterfall row per
// sweep, and every retune is timed. Plot with examples/sweep_plot.py.
//
//   sweep --start 70M --stop 6G --sweeps 10 --out scan   (61.44 MSPS, about 1.3 s per sweep)
//
// Which samples belong to which LO? A fast retune keeps the stream running, so
// the RECONFIGURED mark on the next block is approximate: up to a whole USB
// queue of older samples is still in flight. This example uses the hardware
// time base instead. It reads the device time (time_status?) right after
// och_configure returns; the SDK handles commands one at a time, so the LO
// write and the PLL lock wait finished before that time. Only samples stamped
// at least --guard-us later are used, whatever the USB queue holds. A second
// read just before the retune brackets the retune in sample time.
//
// Outputs (PREFIX from --out):
//   PREFIX.json            axis, settings, and on exit a timing summary
//   PREFIX.waterfall.f32   one row of float32 dBFS per sweep (bins from the json)
//   PREFIX.steps.jsonl     one line per retune with its timing
//   PREFIX.trace.jsonl     power around each retune (first --trace-sweeps sweeps)
//
// --stream writes no files: stdout carries one JSON object per line instead,
// {"type":"meta"|"step"|"row"|"summary",...}; a row's "db" is base64 of its
// little-endian float32 values. Messages go to stderr. sweep_live.py uses this.
//
// Requires OmniCoreHost 0.6+ and framed firmware (hardware time).
// RX only; no transmitter is created.
#include <omnicore/och.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
    using clock_type = std::chrono::steady_clock;
    using cf = std::complex<float>;
    std::atomic<bool> interrupted{false};
    struct Interrupted {};
    constexpr double pi = 3.14159265358979323846;

    double micros(clock_type::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

    // "30.72M", "6G", "500k" -> number.
    double parse_number(const std::string &name, const std::string &text) {
        size_t used = 0;
        double v = 0;
        try {
            v = std::stod(text, &used);
        } catch (...) {
            throw std::invalid_argument("--" + name + ": not a number: " + text);
        }
        const std::string suffix = text.substr(used);
        if (suffix == "k" || suffix == "K")
            v *= 1e3;
        else if (suffix == "M")
            v *= 1e6;
        else if (suffix == "G")
            v *= 1e9;
        else if (!suffix.empty())
            throw std::invalid_argument("--" + name + ": unknown suffix in " + text);
        return v;
    }

    struct Options {
        std::string serial, out = "sweep";
        double start = 70e6, stop = 6e9, rate = 61.44e6, bandwidth = 0, usable = 0.6, gain = 30, guard_us = 50;
        unsigned fft = 2048, average = 16, sweeps = 1, trace_sweeps = 1, dc_bins = 5, transfer_kib = 64;
        bool bracket = true, stream = false;
    };

    void usage() {
        std::cout << "sweep [options]\n"
                     "  --start HZ         first frequency shown (default 70M)\n"
                     "  --stop HZ          last frequency shown (default 6G)\n"
                     "  --rate HZ          sample rate (default 61.44M)\n"
                     "  --bandwidth HZ     RX analog bandwidth (default 0.9 x rate, max 56M)\n"
                     "  --usable F         fraction of each FFT kept, sets the step (default 0.6)\n"
                     "  --fft N            FFT size, power of two (default 2048)\n"
                     "  --average N        FFTs averaged per step (default 16)\n"
                     "  --gain DB          manual RX gain (default 30)\n"
                     "  --guard-us US      samples skipped after the retune (default 50)\n"
                     "  --dc-bins N        bins each side of DC to interpolate over (default 5)\n"
                     "  --transfer-kib N   USB transfer size, multiple of 16 (default 64)\n"
                     "  --sweeps N         number of sweeps, 0 = until Ctrl+C (default 1)\n"
                     "  --trace-sweeps N   sweeps whose retune power traces are saved (default 1)\n"
                     "  --no-bracket       skip the time read before each retune (faster; no trace)\n"
                     "  --stream           JSON lines on stdout instead of output files\n"
                     "  --serial S         device serial\n"
                     "  --out PREFIX       output file prefix (default sweep)\n";
    }

    Options parse(int argc, char **argv) {
        Options o;
        std::map<std::string, std::string> values;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-h" || a == "--help") {
                usage();
                std::exit(0);
            }
            if (a == "--no-bracket") {
                o.bracket = false;
                continue;
            }
            if (a == "--stream") {
                o.stream = true;
                continue;
            }
            if (a.rfind("--", 0) != 0 || i + 1 >= argc) throw std::invalid_argument("bad argument: " + a);
            values[a.substr(2)] = argv[++i];
        }
        auto take = [&](const char *name, auto &field) {
            auto it = values.find(name);
            if (it == values.end()) return;
            if constexpr (std::is_same_v<std::decay_t<decltype(field)>, std::string>)
                field = it->second;
            else if constexpr (std::is_same_v<std::decay_t<decltype(field)>, unsigned>) {
                double v = parse_number(name, it->second);
                if (v < 0 || v != std::floor(v)) throw std::invalid_argument(std::string("--") + name + ": integer");
                field = unsigned(v);
            } else
                field = parse_number(name, it->second);
            values.erase(it);
        };
        take("serial", o.serial);
        take("out", o.out);
        take("start", o.start);
        take("stop", o.stop);
        take("rate", o.rate);
        take("bandwidth", o.bandwidth);
        take("usable", o.usable);
        take("gain", o.gain);
        take("guard-us", o.guard_us);
        take("fft", o.fft);
        take("average", o.average);
        take("sweeps", o.sweeps);
        take("trace-sweeps", o.trace_sweeps);
        take("dc-bins", o.dc_bins);
        take("transfer-kib", o.transfer_kib);
        if (!o.transfer_kib || o.transfer_kib % 16 || o.transfer_kib > 1024)
            throw std::invalid_argument("--transfer-kib must be a multiple of 16 in 16..1024");
        if (!values.empty()) throw std::invalid_argument("unknown option --" + values.begin()->first);
        if (o.fft < 64 || (o.fft & (o.fft - 1))) throw std::invalid_argument("--fft must be a power of two >= 64");
        if (!(o.usable > 0.05 && o.usable <= 1)) throw std::invalid_argument("--usable must be in (0.05, 1]");
        if (o.average < 1) throw std::invalid_argument("--average must be >= 1");
        if (!(o.stop > o.start)) throw std::invalid_argument("--stop must be above --start");
        if (o.stop <= 70e6 || o.start >= 6e9) throw std::invalid_argument("--start..--stop must overlap 70 MHz..6 GHz");
        if (o.bandwidth == 0) o.bandwidth = std::min(56e6, 0.9 * o.rate);
        if (!o.bracket || o.stream) o.trace_sweeps = 0;
        return o;
    }

    // In-place radix-2 FFT.
    void fft(std::vector<cf> &x) {
        const size_t n = x.size();
        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (size_t len = 2; len <= n; len <<= 1) {
            const double a = -2 * pi / double(len);
            const cf wl(float(std::cos(a)), float(std::sin(a)));
            for (size_t i = 0; i < n; i += len) {
                cf w(1, 0);
                for (size_t k = 0; k < len / 2; ++k) {
                    const cf u = x[i + k], v = x[i + k + len / 2] * w;
                    x[i + k] = u + v;
                    x[i + k + len / 2] = u - v;
                    w *= wl;
                }
            }
        }
    }

    uint64_t device_time(och::device &dev) {
        och_time_status t{};
        t.struct_size = sizeof t;
        och::check(och_get_time(dev.native_handle(), &t));
        if (!t.capable) throw std::runtime_error("firmware has no hardware time base (framed firmware required)");
        return t.now;
    }

    // Same rule as the core: the SDK rewrites the gain table across 1.3 / 4 GHz.
    unsigned gain_band(uint64_t hz) { return hz <= 1300000000ULL ? 0 : hz <= 4000000000ULL ? 1 : 2; }
    bool gain_table_step(uint64_t from, uint64_t to) {
        auto near = [](uint64_t hz) {
            for (uint64_t e: {1300000000ULL, 4000000000ULL})
                if ((hz > e ? hz - e : e - hz) < 1000000) return true;
            return false;
        };
        return gain_band(from) != gain_band(to) || near(from) || near(to);
    }

    // Sequential, time-stamped access to the RX stream. Samples left over in a
    // buffer stay for the next step, so no sample time is ever skipped unseen.
    class Reader {
    public:
        explicit Reader(och::stream &s) : rx(s), buffer(16384) {}

        // Next sample and its hardware time.
        cf next(uint64_t &time) {
            if (pos == count) fill();
            time = first_time + pos;
            return buffer[pos++];
        }

        uint64_t gaps = 0, host_dropped = 0, timeouts = 0;

    private:
        void fill() {
            unsigned in_a_row = 0;
            for (;;) {
                if (interrupted.load()) throw Interrupted{};
                size_t got = 0;
                och_metadata meta{};
                int rc = rx.recv(buffer.data(), buffer.size(), got, meta, 250);
                if (rc == OCH_TIMEOUT) {
                    ++timeouts;
                    if (++in_a_row > 20) throw std::runtime_error("no RX data for 5 s");
                    continue;
                }
                och::check(rc);
                if (!got) continue;
                if (!(meta.flags & OCH_META_HAS_TIME)) throw std::runtime_error("RX block without hardware time");
                if (meta.flags & (OCH_META_DEVICE_OVERFLOW | OCH_META_HOST_OVERFLOW)) ++gaps;
                host_dropped += meta.host_dropped_samples;
                first_time = meta.time_ticks;
                count = got;
                pos = 0;
                return;
            }
        }

        och::stream &rx;
        std::vector<cf> buffer;
        size_t count = 0, pos = 0;
        uint64_t first_time = 0;
    };

    struct Step {
        unsigned sweep, index;
        uint64_t frequency;
        bool gain_table;
        double configure_us, time_reads_us, capture_us, total_us, bracket_us;
        uint64_t discarded, restarts;
    };

    double percentile(std::vector<double> v, double p) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))];
    }

    std::string base64(const void *data, size_t size) {
        static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const auto *b = static_cast<const unsigned char *>(data);
        std::string out;
        out.reserve((size + 2) / 3 * 4);
        for (size_t i = 0; i < size; i += 3) {
            const uint32_t v = uint32_t(b[i]) << 16 | (i + 1 < size ? uint32_t(b[i + 1]) << 8 : 0) |
                               (i + 2 < size ? uint32_t(b[i + 2]) : 0);
            out += digits[v >> 18 & 63];
            out += digits[v >> 12 & 63];
            out += i + 1 < size ? digits[v >> 6 & 63] : '=';
            out += i + 2 < size ? digits[v & 63] : '=';
        }
        return out;
    }
} // namespace

int main(int argc, char **argv) {
    try {
        const Options o = parse(argc, argv);
        std::signal(SIGINT, [](int) { interrupted.store(true); });

        // Uniform frequency grid: each step keeps `kept` FFT bins around its LO.
        unsigned kept = unsigned(std::lround(o.fft * o.usable / 2)) * 2;
        kept = std::max(kept, 2u);
        const double bin_hz = o.rate / o.fft;
        const double step_hz = kept * bin_hz;
        const unsigned steps = unsigned(std::ceil((o.stop - o.start) / step_hz));
        std::vector<uint64_t> centers(steps);
        // Whole steps rarely end exactly at --stop: slide the grid back inside the LO range if needed.
        double origin = o.start;
        const double span = step_hz * (steps - 1);
        if (origin + step_hz * 0.5 + span > 6e9) origin = 6e9 - span - step_hz * 0.5;
        if (origin + step_hz * 0.5 < 70e6) origin = 70e6 - step_hz * 0.5;
        for (unsigned k = 0; k < steps; ++k) centers[k] = uint64_t(std::llround(origin + step_hz * (k + 0.5)));
        if (centers.front() < 70000000ULL || centers.back() > 6000000000ULL)
            throw std::invalid_argument("LO would leave 70 MHz..6 GHz: first " + std::to_string(centers.front()) +
                                        " Hz, last " + std::to_string(centers.back()) +
                                        " Hz; move --start/--stop inward");
        const double first_bin_hz = double(centers.front()) - kept / 2.0 * bin_hz;
        const size_t row_bins = size_t(steps) * kept;
        const size_t capture = size_t(o.fft) * o.average;

        och::device dev(o.serial);
        auto config = och::default_config();
        config.sample_rate = uint32_t(std::lround(o.rate));
        config.rx_mask = 1;
        config.tx_mask = 0;
        config.rx_frequency = centers.front();
        config.rx_bandwidth = uint32_t(std::lround(o.bandwidth));
        config.rx_agc = 0;
        config.rx_gain[0] = o.gain;
        dev.configure(config);
        config = dev.config();
        const double rate = config.sample_rate; // applied rate (may differ by a few Hz)
        const uint64_t guard = uint64_t(std::ceil(o.guard_us * 1e-6 * rate));

        // In --stream mode JSON lines go to stdout and human-readable text to stderr.
        std::ostream &info = o.stream ? std::cerr : std::cout;
        auto write_meta = [&](const std::string &summary) {
            std::ofstream f(o.out + ".json", std::ios::trunc);
            f << std::setprecision(12) << "{\n  \"version\": 1,\n  \"sample_rate\": " << rate
              << ",\n  \"bandwidth\": " << config.rx_bandwidth << ",\n  \"gain_db\": " << o.gain
              << ",\n  \"fft\": " << o.fft << ",\n  \"average\": " << o.average << ",\n  \"kept_bins\": " << kept
              << ",\n  \"steps\": " << steps << ",\n  \"step_hz\": " << step_hz << ",\n  \"bin_hz\": " << bin_hz
              << ",\n  \"first_bin_hz\": " << first_bin_hz << ",\n  \"row_bins\": " << row_bins
              << ",\n  \"guard_us\": " << o.guard_us << ",\n  \"bracket\": " << (o.bracket ? "true" : "false")
              << ",\n  \"waterfall\": \"" << o.out << ".waterfall.f32\""
              << ",\n  \"summary\": " << (summary.empty() ? "null" : summary) << "\n}\n";
        };
        auto write_stream_meta = [&] {
            std::cout << std::setprecision(12) << "{\"type\":\"meta\",\"sample_rate\":" << rate
                      << ",\"bandwidth\":" << config.rx_bandwidth << ",\"gain_db\":" << o.gain << ",\"fft\":" << o.fft
                      << ",\"average\":" << o.average << ",\"kept_bins\":" << kept << ",\"steps\":" << steps
                      << ",\"step_hz\":" << step_hz << ",\"bin_hz\":" << bin_hz << ",\"first_bin_hz\":" << first_bin_hz
                      << ",\"row_bins\":" << row_bins << ",\"bracket\":" << (o.bracket ? "true" : "false") << "}"
                      << std::endl;
        };
        std::ofstream waterfall, step_file, trace_log;
        if (o.stream)
            write_stream_meta();
        else {
            write_meta("");
            waterfall.open(o.out + ".waterfall.f32", std::ios::binary | std::ios::trunc);
            step_file.open(o.out + ".steps.jsonl", std::ios::trunc);
            trace_log.open(o.out + ".trace.jsonl", std::ios::trunc);
            if (!waterfall || !step_file || !trace_log) throw std::runtime_error("cannot write output files " + o.out);
        }
        std::ostream &step_log = o.stream ? std::cout : step_file;

        info << "Sweep " << centers.front() / 1e6 << ".." << centers.back() / 1e6 << " MHz LO, " << steps
             << " steps of " << step_hz / 1e6 << " MHz, " << rate / 1e6 << " MSPS, FFT " << o.fft << " x " << o.average
             << " (" << capture / rate * 1e6 << " us per step)\n";

        std::vector<float> window(o.fft);
        double window_sum = 0;
        for (unsigned i = 0; i < o.fft; ++i) {
            window[i] = float(0.5 - 0.5 * std::cos(2 * pi * i / o.fft)); // Hann
            window_sum += window[i];
        }
        const double norm = 1.0 / (window_sum * window_sum * o.average); // full-scale tone -> 0 dBFS

        auto args = och::stream_args(OCH_RX);
        args.format = OCH_FC32;
        // A sample reaches the host only when its whole USB transfer completes: smaller
        // transfers shorten the wait after each retune.
        args.transfer_bytes = o.transfer_kib * 1024;
        och::stream rx(dev, args);
        rx.start();
        Reader reader(rx);

        std::vector<cf> samples(capture), frame(o.fft);
        std::vector<double> power(o.fft);
        std::vector<float> row(row_bins);
        std::vector<Step> log;
        std::vector<double> sweep_seconds;
        uint64_t events_reconfigured = 0, events_gap = 0;
        constexpr unsigned trace_bin = 64;

        for (unsigned sweep = 0; (o.sweeps == 0 || sweep < o.sweeps) && !interrupted.load(); ++sweep) {
            const auto sweep_began = clock_type::now();
            for (unsigned k = 0; k < steps && !interrupted.load(); ++k) {
                Step st{};
                st.sweep = sweep;
                st.index = k;
                st.frequency = centers[k];
                st.gain_table = gain_table_step(config.rx_frequency, centers[k]);
                const bool tracing = sweep < o.trace_sweeps;
                const auto t0 = clock_type::now();

                uint64_t before = 0;
                if (o.bracket) before = device_time(dev);
                const auto t1 = clock_type::now();
                if (config.rx_frequency != centers[k]) {
                    config.rx_frequency = centers[k];
                    dev.configure(config); // fast path: LO only, stream keeps running
                }
                const auto t2 = clock_type::now();
                const uint64_t after = device_time(dev);
                const auto t3 = clock_type::now();
                const uint64_t from = after + guard;
                st.bracket_us = o.bracket ? (after - before) / rate * 1e6 : 0;

                // Skip everything stamped before `from`; take `capture` contiguous samples.
                std::vector<float> trace;
                uint64_t trace_start = before, expected = 0;
                double trace_acc = 0;
                unsigned trace_n = 0;
                size_t have = 0;
                while (have < capture) {
                    uint64_t t = 0;
                    cf s;
                    try {
                        s = reader.next(t);
                    } catch (const Interrupted &) {
                        break; // Ctrl+C: the partial row is dropped below
                    }
                    if (tracing && t >= trace_start) {
                        trace_acc += std::norm(s);
                        if (++trace_n == trace_bin) {
                            trace.push_back(float(10 * std::log10(trace_acc / trace_bin + 1e-20)));
                            trace_acc = 0;
                            trace_n = 0;
                        }
                    }
                    if (t < from) {
                        ++st.discarded;
                        continue;
                    }
                    if (have && t != expected) { // a gap inside the capture: start over
                        ++st.restarts;
                        have = 0;
                    }
                    samples[have++] = s;
                    expected = t + 1;
                }
                if (have < capture) break; // interrupted
                const auto t4 = clock_type::now();

                std::fill(power.begin(), power.end(), 0.0);
                for (unsigned a = 0; a < o.average; ++a) {
                    for (unsigned i = 0; i < o.fft; ++i) frame[i] = samples[size_t(a) * o.fft + i] * window[i];
                    fft(frame);
                    for (unsigned i = 0; i < o.fft; ++i) power[i] += std::norm(frame[i]);
                }
                // fftshift, then interpolate over the DC (LO leakage) bins.
                std::vector<double> shifted(o.fft);
                for (unsigned i = 0; i < o.fft; ++i) shifted[i] = power[(i + o.fft / 2) % o.fft] * norm;
                if (o.dc_bins) {
                    const unsigned c = o.fft / 2, lo = c - o.dc_bins - 1, hi = c + o.dc_bins + 1;
                    for (unsigned i = lo + 1; i < hi; ++i)
                        shifted[i] = shifted[lo] + (shifted[hi] - shifted[lo]) * double(i - lo) / double(hi - lo);
                }
                const unsigned first = o.fft / 2 - kept / 2;
                for (unsigned j = 0; j < kept; ++j)
                    row[size_t(k) * kept + j] = float(10 * std::log10(shifted[first + j] + 1e-20));

                st.configure_us = micros(t2 - t1);
                st.time_reads_us = micros((t1 - t0) + (t3 - t2));
                st.capture_us = micros(t4 - t3);
                st.total_us = micros(t4 - t0);
                log.push_back(st);
                step_log << std::fixed << std::setprecision(1) << (o.stream ? "{\"type\":\"step\"," : "{")
                         << "\"sweep\":" << st.sweep << ",\"step\":" << k << ",\"frequency\":" << st.frequency
                         << ",\"gain_table\":" << (st.gain_table ? 1 : 0) << ",\"configure_us\":" << st.configure_us
                         << ",\"time_reads_us\":" << st.time_reads_us << ",\"capture_us\":" << st.capture_us
                         << ",\"total_us\":" << st.total_us << ",\"bracket_us\":" << st.bracket_us
                         << ",\"discarded\":" << st.discarded << ",\"restarts\":" << st.restarts
                         << ",\"rx_gaps\":" << reader.gaps << ",\"host_dropped\":" << reader.host_dropped << "}\n";
                if (o.stream) std::cout.flush(); // live view: one step at a time
                if (tracing) {
                    trace_log << std::fixed << std::setprecision(2) << "{\"sweep\":" << sweep << ",\"step\":" << k
                              << ",\"frequency\":" << st.frequency << ",\"bin_samples\":" << trace_bin
                              << ",\"sample_rate\":" << rate << ",\"after_us\":" << (after - before) / rate * 1e6
                              << ",\"from_us\":" << (from - before) / rate * 1e6 << ",\"power_db\":[";
                    for (size_t i = 0; i < trace.size(); ++i) trace_log << (i ? "," : "") << trace[i];
                    trace_log << "]}\n";
                }
                och_event e{};
                while (dev.poll_event(e, 0)) {
                    if (e.type == OCH_EVENT_RECONFIGURED) ++events_reconfigured;
                    if (e.type == OCH_EVENT_RX_GAP || e.type == OCH_EVENT_HOST_OVERFLOW)
                        events_gap += e.count ? e.count : 1;
                }
            }
            if (interrupted.load()) break; // drop a partial row
            sweep_seconds.push_back(std::chrono::duration<double>(clock_type::now() - sweep_began).count());
            if (o.stream) {
                std::cout << std::fixed << std::setprecision(4) << "{\"type\":\"row\",\"sweep\":" << sweep
                          << ",\"seconds\":" << sweep_seconds.back() << ",\"db\":\""
                          << base64(row.data(), row.size() * sizeof(float)) << "\"}" << std::endl;
            } else {
                waterfall.write(reinterpret_cast<const char *>(row.data()),
                                std::streamsize(row.size() * sizeof(float)));
                waterfall.flush();
                step_file.flush();
                trace_log.flush();
            }
            info << "sweep " << sweep << ": " << std::fixed << std::setprecision(3) << sweep_seconds.back() << " s, "
                 << std::setprecision(0) << steps / sweep_seconds.back() << " steps/s\n";
        }

        rx.stop();
        const auto stats = rx.stats();

        // Summary. The first step of a sweep jumps back across the band, and
        // gain-table steps rewrite SPI tables, so report them separately.
        std::vector<double> same, table, total, bracket;
        for (const auto &s: log) {
            if (s.sweep == 0 && s.index == 0) continue; // initial LO already set
            (s.gain_table ? table : same).push_back(s.configure_us);
            total.push_back(s.total_us);
            if (o.bracket) bracket.push_back(s.bracket_us);
        }
        auto stats_json = [](const std::vector<double> &v) {
            std::ostringstream s;
            s << std::fixed << std::setprecision(1) << "{\"n\":" << v.size() << ",\"median\":" << percentile(v, 0.5)
              << ",\"p90\":" << percentile(v, 0.9)
              << ",\"max\":" << (v.empty() ? 0 : *std::max_element(v.begin(), v.end())) << "}";
            return s.str();
        };
        double sweep_median = percentile(sweep_seconds, 0.5);
        std::ostringstream summary;
        summary << std::fixed << std::setprecision(4) << "{\"sweeps\":" << sweep_seconds.size()
                << ",\"sweep_seconds_median\":" << sweep_median
                << ",\"steps_per_second\":" << (sweep_median > 0 ? steps / sweep_median : 0)
                << ",\"configure_us_same_band\":" << stats_json(same)
                << ",\"configure_us_gain_table\":" << stats_json(table) << ",\"step_total_us\":" << stats_json(total)
                << ",\"bracket_us\":" << stats_json(bracket) << ",\"rx_gaps\":" << reader.gaps
                << ",\"gap_events\":" << events_gap << ",\"host_dropped_samples\":" << reader.host_dropped
                << ",\"reconfigured_events\":" << events_reconfigured
                << ",\"device_overflows\":" << stats.device_overflows
                << ",\"transfer_errors\":" << stats.transfer_errors << "}";
        if (o.stream)
            std::cout << "{\"type\":\"summary\",\"summary\":" << summary.str() << "}" << std::endl;
        else
            write_meta(summary.str());

        info << std::fixed << std::setprecision(0) << "retune (och_configure) same band: median "
             << percentile(same, 0.5) << " us, p90 " << percentile(same, 0.9) << " us; gain table: median "
             << percentile(table, 0.5) << " us (" << table.size() << " steps)\n";
        if (o.bracket) info << "retune bracket in sample time: median " << percentile(bracket, 0.5) << " us\n";
        info << "step total: median " << percentile(total, 0.5) << " us; RX gaps " << reader.gaps << ", host dropped "
             << reader.host_dropped << ", device overflows " << stats.device_overflows << '\n';
        if (!o.stream)
            info << "wrote " << o.out << ".json, .waterfall.f32, .steps.jsonl"
                 << (o.trace_sweeps ? ", .trace.jsonl" : "") << '\n';
        rx.close();
        dev.close();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
