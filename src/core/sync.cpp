// Multi-board synchronisation (firmware SDK_CMD_1.2 with FPGA OC_SYNC_AXI 1.0).
//
// Per board: sync_status?, ref_out=, trig_out=, trig_edge=, trig_width=,
// trig_period=, trig_pulse!, trig_at_pps!, trig_disarm!, sync_loopback=,
// sync_clear!, mcs_step=. Group: och_multi_sync() orchestrates reference lock
// check, AD9361 multi-chip sync and a common time load (see och.h).
#include "internal.hpp"
#include <charconv>
#include <chrono>
#include <thread>
using namespace och::core;

namespace {
std::string field(const std::string& s, const std::string& name) {
    size_t at = 0;
    while ((at = s.find(name + "=", at)) != std::string::npos) {
        if (!at || s[at - 1] == ' ' || s[at - 1] == '\t' || s[at - 1] == '\r' || s[at - 1] == '\n') {
            at += name.size() + 1;
            return s.substr(at, s.find_first_of(" \t\r\n", at) - at);
        }
        ++at;
    }
    throw Failure(OCH_PROTOCOL, "SDK reply missing " + name);
}
uint64_t u64(const std::string& s) {
    uint64_t v = 0;
    auto p = std::from_chars(s.data(), s.data() + s.size(), v);
    require(p.ec == std::errc() && p.ptr == s.data() + s.size(), OCH_PROTOCOL, "invalid integer in SDK reply");
    return v;
}
uint32_t u32f(const std::string& r, const char* name) { return uint32_t(u64(field(r, name))); }
void sleep_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint64_t now_ms() { return now_ns() / 1000000u; }

constexpr uint32_t REF_HZ = 40000000u;
constexpr uint32_t LOCK_PERIOD = 4000000u;   // 100 ms TRIG while checking the chain

std::string board_name(size_t i) { return i ? "board " + std::to_string(i) : std::string("master"); }

// Locks one device for one control transaction.
template<class F> auto with(och_device* d, F f) {
    std::lock_guard<std::mutex> lock(d->state_mutex);
    return f(*d);
}
}

och_sync_status och::core::read_sync_status(och_device& d) {
    och_sync_status s{};
    s.struct_size = sizeof s;
    s.mcs_step = -1;
    if (!d.supports_capability("sync")) return s;
    const auto r = d.command("sync_status?");
    s.capable = u32f(r, "capable");
    if (!s.capable) return s;
    s.version = uint32_t(std::stoul(field(r, "version"), nullptr, 16));
    s.ref40_present = u32f(r, "ref40_present");
    s.ref40_hz = u32f(r, "ref40_hz");
    s.ref_out = u32f(r, "ref_out");
    s.trig_out = u32f(r, "trig_out");
    s.trig_falling = field(r, "trig_edge") == "falling";
    s.trig_width = u32f(r, "trig_width");
    s.trig_period = u32f(r, "trig_period");
    s.armed = u32f(r, "armed");
    s.loopback = u32f(r, "loopback");
    s.time_pps_sync = field(r, "time_pps") == "sync";
    s.sync_count = u32f(r, "sync_count");
    s.sync_interval = u32f(r, "sync_interval");
    s.pps_count = u32f(r, "pps_count");
    s.pps_interval = u32f(r, "pps_interval");
    s.trig_count = u32f(r, "trig_count");
    s.mcs_step = int32_t(std::stol(field(r, "mcs_step")));
    s.mcs_done = u32f(r, "mcs_done");
    return s;
}

void och::core::apply_sync_config(och_device& d, const och_sync_config& c) {
    d.require_capability("sync");
    require(c.trig_width <= 65535, OCH_INVALID, "trig_width must be 0..65535 cycles");
    require(c.trig_period == 0 || c.trig_period >= 1000, OCH_INVALID, "trig_period must be 0 or >= 1000 cycles");
    const auto cur = read_sync_status(d);
    require(cur.ref40_present, OCH_IO, "40 MHz reference (CLK_FB_FPGA) not detected by the FPGA");
    // Order keeps width < period valid in between.
    if (c.trig_period == 0 || (cur.trig_period && c.trig_width && c.trig_width >= cur.trig_period))
        d.command("trig_period=0");
    if (c.trig_width) d.command("trig_width=" + std::to_string(c.trig_width));
    d.command(std::string("trig_edge=") + (c.trig_falling ? "falling" : "rising"));
    d.command("sync_loopback=" + std::to_string(c.loopback ? 1 : 0));
    d.command("ref_out=" + std::to_string(c.ref_out ? 1 : 0));
    d.command("trig_out=" + std::to_string(c.trig_out ? 1 : 0));
    if (c.trig_period) d.command("trig_period=" + std::to_string(c.trig_period));
}

void och::core::sync_trigger(och_device& d, uint32_t mode) {
    d.require_capability("sync");
    require(mode <= OCH_SYNC_TRIGGER_DISARM, OCH_INVALID, "trigger mode must be 0..2");
    d.command(mode == OCH_SYNC_TRIGGER_NOW ? "trig_pulse!" : mode == OCH_SYNC_TRIGGER_AT_PPS ? "trig_at_pps!" : "trig_disarm!");
}

namespace {
struct Group {
    och_device* const* dev;
    size_t n;
    std::string cmd(size_t i, const std::string& c, uint32_t timeout = 5000) {
        try { return with(dev[i], [&](och_device& d) { return d.command(c, timeout); }); }
        catch (const Failure& e) { throw Failure(e.code, board_name(i) + ": " + c + ": " + e.what()); }
    }
    void all(const std::string& c) { for (size_t i = 0; i < n; ++i) cmd(i, c); }
    och_sync_status status(size_t i) { return with(dev[i], [](och_device& d) { return read_sync_status(d); }); }
    och_time_status time(size_t i) { return with(dev[i], [](och_device& d) { return d.get_time(); }); }
};

// Waits for the next PPS_EXT edge seen by board 0 and returns soon after it,
// leaving most of a second for commands that must land before the next one.
void after_pps_edge(Group& g, uint64_t deadline) {
    const auto first = g.status(0).pps_count;
    while (g.status(0).pps_count == first) {
        require(now_ms() < deadline, OCH_TIMEOUT, "no PPS edge on the master's PPS_EXT within the timeout");
        sleep_ms(5);
    }
}
}

void och_default_multi_sync_args(och_multi_sync_args* a) {
    if (!a) return;
    *a = {};
    a->struct_size = sizeof *a;
    a->mode = OCH_SYNC_CHAIN;
    a->mcs = 1;
    a->lock_timeout_ms = 15000;
}

void och_default_sync_config(och_sync_config* c) {
    if (!c) return;
    *c = {};
    c->struct_size = sizeof *c;
    c->trig_width = 80;
}

void och::core::multi_sync(och_device* const* devices, size_t n, const och_multi_sync_args& a,
                           och_multi_sync_result* results) {
    require(devices && n >= 1 && n <= 64, OCH_INVALID, "1..64 devices required");
    require(a.mode <= OCH_SYNC_COMMON, OCH_INVALID, "mode must be OCH_SYNC_CHAIN or OCH_SYNC_COMMON");
    require(!a.loopback || (a.mode == OCH_SYNC_CHAIN && n == 1), OCH_INVALID, "loopback is a single-board CHAIN test");
    for (size_t i = 0; i < n; ++i) {
        require(devices[i] != nullptr, OCH_INVALID, "null device");
        for (size_t j = 0; j < i; ++j) require(devices[i] != devices[j], OCH_INVALID, "device listed twice");
        with(devices[i], [&](och_device& d) {
            try { d.require_capability("sync"); d.require_capability("hw_time"); }
            catch (const Failure& e) { throw Failure(e.code, board_name(i) + ": " + e.what()); }
            require(!d.rx_on && !d.tx_on, OCH_BUSY, board_name(i) + ": stop RX/TX streams before synchronising");
            return 0;
        });
    }
    Group g{devices, n};
    std::vector<och_multi_sync_result> res(n);
    for (auto& r : res) r.struct_size = sizeof r;
    const uint64_t timeout = a.lock_timeout_ms ? a.lock_timeout_ms : 15000;
    const bool chain = a.mode == OCH_SYNC_CHAIN;

    // 1. Reference sources and outputs.
    for (size_t i = 0; i < n; ++i) {
        const unsigned ref = chain ? (i ? 1u : (a.master_external_ref ? 1u : 0u)) : 1u;
        with(devices[i], [&](och_device& d) { d.set_reference_source(ref); return 0; });
        och_sync_config c{};
        c.struct_size = sizeof c;
        c.ref_out = chain ? 1 : 0;               // every board forwards the chain
        c.trig_out = chain ? (i == 0) : a.mcs;    // COMMON: own TRIG -> own J1 for MCS
        c.trig_width = 80;
        c.loopback = a.loopback;
        with(devices[i], [&](och_device& d) { apply_sync_config(d, c); return 0; });
        g.cmd(i, "trig_disarm!");
        g.cmd(i, "sync_clear!");
    }

    // 2. Lock check. CHAIN: the master's 100 ms TRIG must arrive at every
    //    board with an interval of exactly LOCK_PERIOD of that board's own
    //    40 MHz. COMMON: every board's PPS interval must be exactly 40e6.
    {
        const uint32_t expect = chain ? LOCK_PERIOD : REF_HZ;
        const uint64_t deadline = now_ms() + timeout + (chain ? 0 : 3000);
        if (chain) g.cmd(0, "trig_period=" + std::to_string(LOCK_PERIOD));
        std::vector<unsigned> good(n, 0);
        std::vector<uint32_t> last_edges(n, 0);
        for (;;) {
            bool done = true;
            for (size_t i = 0; i < n; ++i) {
                const auto s = g.status(i);
                const uint32_t edges = chain ? s.sync_count : s.pps_count;
                const uint32_t iv = chain ? s.sync_interval : s.pps_interval;
                res[i].edges = edges;
                res[i].interval = iv;
                res[i].interval_error = int32_t(int64_t(iv) - int64_t(expect));
                if (edges != last_edges[i] && edges >= 2) {
                    good[i] = (iv + 1 >= expect && iv <= expect + 1) ? good[i] + 1 : 0;
                    last_edges[i] = edges;
                }
                res[i].locked = good[i] >= 3;
                done = done && res[i].locked;
            }
            if (done) break;
            if (now_ms() > deadline) {
                if (chain) g.cmd(0, "trig_period=0");
                for (size_t i = 0; i < n; ++i) {
                    if (res[i].locked) continue;
                    if (res[i].edges == 0)
                        throw Failure(OCH_TIMEOUT, board_name(i) + (chain
                            ? ": no SYNC edges; wire the master's TRIG (J9) through a 1:N splitter to every board's AD9361_SYNC (J1)"
                            : ": no PPS edges on PPS_EXT"));
                    throw Failure(OCH_TIMEOUT, board_name(i) + ": reference not locked, interval " +
                        std::to_string(res[i].interval) + " cycles, expected " + std::to_string(expect) +
                        (chain ? " (check the REF_OUT J13 -> 10M_EXT J3 chain)" : " (check the 10 MHz on J3)"));
                }
            }
            sleep_ms(50);
        }
        if (chain) g.cmd(0, "trig_period=0");
        sleep_ms(150);   // the last periodic pulse ends
    }

    // 3. AD9361 multi-chip synchronisation: steps on every board, SYNC_IN
    //    pulses at steps 2 and 4 from the master TRIG (CHAIN) or every board's
    //    TRIG at the same PPS edge (COMMON).
    if (a.mcs) {
        for (unsigned step = 0; step <= 5; ++step) {
            g.all("mcs_step=" + std::to_string(step));
            if (step == 2 || step == 4) {
                std::vector<uint32_t> before(n);
                for (size_t i = 0; i < n; ++i) before[i] = chain ? g.status(i).sync_count : g.status(i).trig_count;
                if (chain) g.cmd(0, "trig_pulse!");
                else {
                    after_pps_edge(g, now_ms() + 3000);
                    g.all("trig_at_pps!");
                }
                const uint64_t deadline = now_ms() + (chain ? 500 : 2500);
                for (size_t i = 0; i < n; ++i) {
                    for (;;) {
                        const auto s = g.status(i);
                        if ((chain ? s.sync_count : s.trig_count) == before[i] + 1) break;
                        if (now_ms() > deadline) {
                            g.all("mcs_step=5");
                            throw Failure(OCH_TIMEOUT, board_name(i) + ": MCS step " + std::to_string(step) +
                                          " pulse not seen (SYNC wiring?)");
                        }
                        sleep_ms(5);
                    }
                }
            }
            sleep_ms(100);
        }
        for (auto& r : res) r.mcs_done = 1;
    }

    // 4. Common time: every board loads start_time at the same edge.
    {
        const std::string load = "time_next_pps=" + std::to_string(a.start_time);
        g.all(chain ? "time_source=sync" : "time_source=external");
        if (chain) {
            g.all(load);
            g.cmd(0, "trig_pulse!");
        } else {
            after_pps_edge(g, now_ms() + 3000);
            g.all(load);
        }
        const uint64_t deadline = now_ms() + (chain ? 1000 : 2500);
        for (size_t i = 0; i < n; ++i) {
            for (;;) {
                const auto t = g.time(i);
                if (t.valid && !t.armed) {
                    res[i].time_valid = 1;
                    res[i].tick_rate = t.tick_rate;
                    res[i].time_at_sync = t.last_pps;
                    break;
                }
                require(now_ms() < deadline, OCH_TIMEOUT, board_name(i) + ": time load edge not seen");
                sleep_ms(5);
            }
            require(res[i].time_at_sync == a.start_time, OCH_PROTOCOL,
                    board_name(i) + ": time_last_pps after the load is not start_time");
        }
    }

    // 5. Optional periodic TRIG: the shared "PPS" keeps every board's
    //    last_pps comparable and SYNC_INTERVAL keeps showing the lock.
    if (chain && a.monitor_period) {
        require(a.monitor_period >= 1000, OCH_INVALID, "monitor_period must be 0 or >= 1000 cycles");
        g.cmd(0, "trig_period=" + std::to_string(a.monitor_period));
    }
    if (results)
        for (size_t i = 0; i < n; ++i) results[i] = res[i];
}
