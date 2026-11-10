// Hardware test of the OmniCore time base and framed timed RX/TX.
// Requires FPGA CH32H417_IP 1.2 + matching SDK. Uses the AD9361 data-port
// loopback (TX words -> RX words), so no RF cable is needed; TX attenuation
// is set to the maximum.
#include <omnicore/och.h>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            ++failures;                                                                                                \
            std::printf("  FAIL: " __VA_ARGS__);                                                                       \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)
static void ok(int rc, const char *what) {
    if (rc != OCH_OK) {
        std::printf("FATAL %s rc=%d: %s\n", what, rc, och_last_error());
        std::exit(2);
    }
}
static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static och_time_status get_time(och_device *d) {
    och_time_status t{};
    t.struct_size = sizeof t;
    ok(och_get_time(d, &t), "get_time");
    return t;
}
static std::string cmd(och_device *d, const std::string &c) {
    char reply[4096] = {};
    ok(och_command(d, c.c_str(), reply, sizeof reply, 5000), c.c_str());
    return reply;
}
static uint32_t g_rate = 30720000;
static och_config base_config(uint32_t framing) {
    och_config c;
    och_default_config(&c);
    c.sample_rate = g_rate;
    c.rx_mask = 3;
    c.tx_mask = 3;
    c.usb_framing = framing;
    c.rx_frequency = c.tx_frequency = 2400000000ULL;
    c.tx_attenuation[0] = c.tx_attenuation[1] = 89.75;
    c.rx_agc = 0;
    return c;
}

int main(int argc, char **argv) {
    if (argc > 1) g_rate = uint32_t(std::strtoul(argv[1], nullptr, 10));
    och_device *d = nullptr;
    ok(och_open(nullptr, &d), "open");
    och_device_info info{};
    info.struct_size = sizeof info;
    ok(och_get_info(d, &info), "info");
    std::printf("device %s firmware=%s hardware_timestamps=%u\n", info.serial, info.firmware, info.hardware_timestamps);
    CHECK(info.hardware_timestamps == 1, "hardware_timestamps capability");
    auto cfg = base_config(1);
    ok(och_configure(d, &cfg), "configure framed");
    const uint32_t fs = cfg.sample_rate;
    std::printf("sample rate %u\n", fs);

    // ------------------------------------------------------------------ 1
    std::printf("[1] time base: set now, rate\n");
    ok(och_set_time(d, 1000000, 0), "set_time now");
    auto t0 = get_time(d);
    auto h0 = std::chrono::steady_clock::now();
    std::printf("  version=0x%08x valid=%u now=%" PRIu64 " tick_rate=%u\n", t0.fpga_version, t0.valid, t0.now,
                t0.tick_rate);
    CHECK(t0.capable && t0.valid && t0.fpga_version == 0x01030001u, "time status after set");
    CHECK(t0.now >= 1000000 && t0.now < 1000000 + fs / 2, "time after set_time(1e6) = %" PRIu64, t0.now);
    sleep_ms(1000);
    auto t1 = get_time(d);
    double host_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - h0).count();
    double rate = double(t1.now - t0.now) / host_s;
    std::printf("  counter rate %.0f ticks/s (sample rate %u)\n", rate, fs);
    CHECK(std::abs(rate - fs) < fs * 0.02, "counter rate");

    // ------------------------------------------------------------------ 2
    std::printf("[2] internal PPS from AD9361 CLK_OUT (40 MHz) and next-PPS load\n");
    ok(och_set_time_source(d, OCH_TIME_SOURCE_INTERNAL), "time source internal");
    sleep_ms(2300);
    auto p0 = get_time(d);
    std::printf("  ref40_present=%u pps_count=%u last_pps=%" PRIu64 "\n", p0.ref_present, p0.pps_count, p0.last_pps);
    CHECK(p0.ref_present == 1, "AD9361 CLK_OUT present on U10");
    CHECK(p0.pps_count >= 2, "internal PPS edges");
    och_time_status p1 = p0;
    for (int i = 0; i < 30 && p1.pps_count == p0.pps_count; ++i) {
        sleep_ms(50);
        p1 = get_time(d);
    }
    int64_t spacing = int64_t(p1.last_pps - p0.last_pps) / int64_t(p1.pps_count - p0.pps_count);
    std::printf("  PPS spacing = %" PRId64 " samples (expect %u: PPS and sample clock share the 40 MHz reference)\n",
                spacing, fs);
    CHECK(std::llabs(spacing - int64_t(fs)) <= 1, "PPS spacing in samples");
    const uint64_t pps_value = 5000000000ULL;
    ok(och_set_time(d, pps_value, 1), "set_time next pps");
    auto a = get_time(d);
    CHECK(a.armed == 1, "armed before PPS");
    sleep_ms(1300);
    auto b = get_time(d);
    std::printf("  after PPS: armed=%u last_pps=%" PRIu64 " now=%" PRIu64 "\n", b.armed, b.last_pps, b.now);
    CHECK(b.armed == 0 && b.valid == 1, "PPS load");
    // last_pps is the load value or a later PPS that followed it by whole seconds.
    CHECK(b.last_pps >= pps_value && (b.last_pps - pps_value) % fs <= 1, "last_pps relative to loaded value");
    CHECK(b.now > b.last_pps && b.now - b.last_pps < fs * 2ULL, "time continues after PPS load");

    // ------------------------------------------------------------------ 3
    std::printf("[3] framed RX: continuous timestamps for 2 s\n");
    {
        och_stream_args ra;
        och_default_stream_args(&ra, OCH_RX);
        ra.format = OCH_SC16;
        och_stream *rx = nullptr;
        ok(och_create_stream(d, &ra, &rx), "rx create");
        ok(och_start(rx), "rx start");
        std::vector<int16_t> buf(2 * 2 * 65536);
        uint64_t total = 0, expect = 0, gaps = 0, segments = 0, mismatches = 0;
        bool first = true;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end) {
            size_t got = 0;
            och_metadata m{};
            m.struct_size = sizeof m;
            int rc = och_recv(rx, buf.data(), 65536, &got, &m, 500);
            if (rc == OCH_TIMEOUT) continue;
            ok(rc, "recv");
            if (!(m.flags & OCH_META_HAS_TIME)) {
                ++mismatches;
                continue;
            }
            if (first || (m.flags & OCH_META_START)) ++segments;
            if (m.flags & OCH_META_DEVICE_OVERFLOW)
                ++gaps;
            else if (!first && m.time_ticks != expect)
                ++mismatches;
            first = false;
            expect = m.time_ticks + got;
            total += got;
        }
        och_stats st{};
        st.struct_size = sizeof st;
        ok(och_get_stats(rx, &st), "stats");
        std::printf("  samples=%" PRIu64 " (%.2f MS/s) blocks=%" PRIu64 " segments=%" PRIu64 " gaps=%" PRIu64
                    " time_mismatch=%" PRIu64 " host_drops=%" PRIu64 "\n",
                    total, total / 2e6, st.headers, segments, gaps, mismatches, st.host_dropped_samples);
        CHECK(mismatches == 0, "RX timestamps continuous");
        CHECK(total > uint64_t(fs) * 3 / 2, "RX throughput");
        ok(och_destroy_stream(rx), "rx destroy");
    }

    // ------------------------------------------------------------------ 4
    std::printf("[4] timed RX command: start at now+0.5 s, 100000 samples\n");
    {
        auto now = get_time(d).now;
        const uint64_t start = now + fs / 2;
        och_stream_args ra;
        och_default_stream_args(&ra, OCH_RX);
        ra.format = OCH_SC16;
        ra.start_flags = OCH_RX_TIMED;
        ra.num_samples = 100000;
        ra.start_time_lo = uint32_t(start);
        ra.start_time_hi = uint32_t(start >> 32);
        och_stream *rx = nullptr;
        ok(och_create_stream(d, &ra, &rx), "rx create");
        ok(och_start(rx), "rx start");
        std::vector<int16_t> buf(2 * 2 * 65536);
        uint64_t total = 0, first_time = 0;
        bool have = false, eob = false;
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!eob && std::chrono::steady_clock::now() < end) {
            size_t got = 0;
            och_metadata m{};
            m.struct_size = sizeof m;
            int rc = och_recv(rx, buf.data(), 65536, &got, &m, 200);
            if (rc == OCH_TIMEOUT) continue;
            ok(rc, "recv");
            if (!have) {
                first_time = m.time_ticks;
                have = true;
                std::printf("  first block flags=0x%x late=%u\n", m.flags, !!(m.flags & OCH_META_LATE));
            }
            total += got;
            if (m.flags & OCH_META_END_OF_BURST) eob = true;
        }
        std::printf("  requested start=%" PRIu64 " first sample time=%" PRIu64 " samples=%" PRIu64 " eob=%d\n", start,
                    first_time, total, eob);
        CHECK(have && first_time == start, "timed RX starts exactly at the requested sample");
        CHECK(total == 100000 && eob, "timed RX sample count and END_OF_BURST");
        // A second command on the same stream, already late.
        ok(och_rx_command(rx, 0, get_time(d).now - 1000, 5000), "late rx command");
        bool late = false;
        uint64_t n2 = 0;
        end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < end) {
            size_t got = 0;
            och_metadata m{};
            m.struct_size = sizeof m;
            int rc = och_recv(rx, buf.data(), 65536, &got, &m, 200);
            if (rc == OCH_TIMEOUT) continue;
            ok(rc, "recv");
            late |= (m.flags & OCH_META_LATE) != 0;
            n2 += got;
            if (m.flags & OCH_META_END_OF_BURST) break;
        }
        std::printf("  late command: LATE flag=%d samples=%" PRIu64 "\n", late, n2);
        CHECK(late && n2 == 5000, "late RX command flagged and still delivered");
        ok(och_destroy_stream(rx), "rx destroy");
    }

    // ------------------------------------------------------------------ 5
    std::printf("[5] timed TX bursts through the AD9361 data-port loopback\n");
    std::printf("  %s", cmd(d, "bist_loopback=1").c_str());
    ok(och_configure(d, &cfg), "configure after loopback"); // raw mutations require re-configure
    {
        std::string lb = cmd(d, "bist_loopback?");
        std::printf("  after configure: %s", lb.c_str());
        CHECK(lb.find("bist_loopback=1") != std::string::npos, "loopback survives configure");
        while (true) {
            och_event e{};
            e.struct_size = sizeof e;
            if (och_poll_event(d, &e, 0) != OCH_OK) break;
        }
        och_stream_args ra;
        och_default_stream_args(&ra, OCH_RX);
        ra.format = OCH_SC16;
        och_stream *rx = nullptr;
        ok(och_create_stream(d, &ra, &rx), "rx create");
        och_stream_args ta;
        och_default_stream_args(&ta, OCH_TX);
        ta.format = OCH_SC16;
        och_stream *tx = nullptr;
        ok(och_create_stream(d, &ta, &tx), "tx create");
        const size_t burst = 3000;
        std::vector<int16_t> tone(burst * 4);
        // ch0: I = sample number (ramp), Q = constant marker; ch1: constants.
        for (size_t k = 0; k < burst; ++k) {
            tone[k * 4 + 0] = int16_t((int(k % 2000) + 16) * 16);
            tone[k * 4 + 1] = int16_t(1500 * 16);
            tone[k * 4 + 2] = int16_t(-1200 * 16);
            tone[k * 4 + 3] = int16_t(300 * 16);
        }
        ok(och_start(rx), "rx start");
        const int bursts = 6;
        std::vector<uint64_t> tx_time(bursts);
        uint64_t base = get_time(d).now + fs; // first burst 1 s ahead
        for (int k = 0; k < bursts; ++k) tx_time[k] = base + uint64_t(k) * (fs / 5) + uint64_t(k) * 1237;
        auto send = [&](int k, uint64_t t) {
            och_tx_metadata md{};
            md.struct_size = sizeof md;
            md.flags = OCH_TX_HAS_TIME | OCH_TX_START_OF_BURST | OCH_TX_END_OF_BURST;
            md.time_ticks = t;
            size_t acc = 0;
            ok(och_send_burst(tx, tone.data(), burst, &md, &acc, 2000), "send_burst");
            CHECK(acc == burst, "burst %d accepted", k);
        };
        send(0, tx_time[0]);
        ok(och_start(tx), "tx start");
        for (int k = 1; k < bursts; ++k) send(k, tx_time[k]);
        // Late burst: must not be transmitted; produces TX_LATE.
        sleep_ms(50);
        send(99, get_time(d).now - 2000);
        // Collect RX for 2.6 s and find each burst's first loopback sample.
        std::vector<int16_t> buf(2 * 2 * 65536);
        std::vector<uint64_t> rx_edge;
        bool high = false;
        uint64_t run_start = 0, run_len = 0;
        std::vector<uint64_t> run_lens;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2600);
        while (std::chrono::steady_clock::now() < end) {
            size_t got = 0;
            och_metadata m{};
            m.struct_size = sizeof m;
            int rc = och_recv(rx, buf.data(), 65536, &got, &m, 200);
            if (rc == OCH_TIMEOUT) continue;
            ok(rc, "recv");
            for (size_t i = 0; i < got; ++i) {
                bool on = std::abs(int(buf[i * 4 + 1])) > 8000; // Q marker
                if (on && !high) {
                    rx_edge.push_back(m.time_ticks + i);
                    run_start = m.time_ticks + i;
                    run_len = 0;
                    if (rx_edge.size() == 1)
                        std::printf("  first RX frames: [%d %d %d %d] [%d %d %d %d] (I/16-16 = TX sample number)\n",
                                    buf[i * 4] / 16, buf[i * 4 + 1] / 16, buf[i * 4 + 2] / 16, buf[i * 4 + 3] / 16,
                                    buf[i * 4 + 4] / 16, buf[i * 4 + 5] / 16, buf[i * 4 + 6] / 16, buf[i * 4 + 7] / 16);
                }
                if (!on && high && rx_edge.size() == 1 && i)
                    std::printf("  last RX frame of burst 0: [%d %d %d %d]\n", buf[i * 4 - 4] / 16, buf[i * 4 - 3] / 16,
                                buf[i * 4 - 2] / 16, buf[i * 4 - 1] / 16);
                if (on) ++run_len;
                if (!on && high) run_lens.push_back(run_len);
                high = on;
            }
        }
        (void) run_start;
        std::printf("  loopback bursts seen=%zu\n", rx_edge.size());
        CHECK(rx_edge.size() == size_t(bursts), "number of bursts received (late burst must be absent)");
        int64_t lat0 = 0;
        for (size_t k = 0; k < rx_edge.size() && k < size_t(bursts); ++k) {
            int64_t lat = int64_t(rx_edge[k] - tx_time[k]);
            if (!k) lat0 = lat;
            std::printf("  burst %zu: TX time=%" PRIu64 " RX edge=%" PRIu64 " latency=%" PRId64
                        " samples length=%" PRIu64 "\n",
                        k, tx_time[k], rx_edge[k], lat, k < run_lens.size() ? run_lens[k] : 0);
            CHECK(lat == lat0, "TX->RX latency must be identical for every burst");
            CHECK(k >= run_lens.size() || run_lens[k] == burst, "burst length");
        }
        std::printf("  constant TX(time) -> RX(time) loopback latency = %" PRId64 " samples (%.3f us)\n", lat0,
                    lat0 * 1e6 / fs);
        // Events
        int acks = 0, lates = 0, others = 0;
        for (int i = 0; i < 200; ++i) {
            och_event e{};
            e.struct_size = sizeof e;
            if (och_poll_event(d, &e, 20) != OCH_OK) break;
            if (e.type == OCH_EVENT_TX_BURST_ACK)
                ++acks;
            else if (e.type == OCH_EVENT_TX_LATE) {
                ++lates;
                std::printf("  TX_LATE event time=%" PRIu64 "\n", e.hw_time);
            } else if (e.type >= OCH_EVENT_TX_BURST_ACK) {
                ++others;
                std::printf("  event %u: %s\n", e.type, e.message);
            }
        }
        std::printf("  events: burst_ack=%d tx_late=%d other_timed=%d\n", acks, lates, others);
        CHECK(acks == bursts, "one burst ACK per burst");
        CHECK(lates == 1, "late burst reported");
        ok(och_destroy_stream(tx), "tx destroy");
        ok(och_destroy_stream(rx), "rx destroy");
    }
    std::printf("  %s", cmd(d, "bist_loopback=0").c_str());

    // ------------------------------------------------------------------ 6
    std::printf("[6] default configuration (usb_framing not set) is framed with hardware time\n");
    {
        auto lc = base_config(0);
        ok(och_configure(d, &lc), "configure default");
        och_stream_args ra;
        och_default_stream_args(&ra, OCH_RX);
        ra.format = OCH_SC16;
        och_stream *rx = nullptr;
        ok(och_create_stream(d, &ra, &rx), "rx create");
        ok(och_start(rx), "rx start");
        std::vector<int16_t> buf(2 * 2 * 65536);
        uint64_t total = 0, untimed = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < end) {
            size_t got = 0;
            och_metadata m{};
            m.struct_size = sizeof m;
            if (och_recv(rx, buf.data(), 65536, &got, &m, 200) == OCH_OK) {
                total += got;
                if (!(m.flags & OCH_META_HAS_TIME) || (m.flags & OCH_META_TIME_UNKNOWN)) ++untimed;
            }
        }
        std::printf("  samples=%" PRIu64 " buffers_without_time=%" PRIu64 "\n", total, untimed);
        CHECK(total > fs / 4 && untimed == 0, "default stream carries hardware time");
        ok(och_destroy_stream(rx), "rx destroy");
    }
    och_close(d);
    std::printf("HW TIMESTAMP TEST: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
