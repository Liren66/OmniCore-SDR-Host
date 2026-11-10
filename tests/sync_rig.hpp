// Model of the multi-board sync hardware for offline tests: N boards on FakeLinks, the wiring
// between them, and a clock that produces TRIG / SYNC / PPS edges.
//
// Time is virtual and counted in 40 MHz cycles. It advances by SyncBus::step each time a board
// answers sync_status? or time_status?, and never otherwise: what a test sees depends on the
// order of commands, not on how fast the host runs.
#pragma once
#include "test_support.hpp"
#include <memory>

namespace och::test {
    // One board's sync and time registers, plus what the test wires or breaks.
    struct SyncBoard {
        // --- Wiring and faults, set by the test before the run.
        bool sync_wired = true;           // the group TRIG reaches this board's SYNC input
        bool pps_wired = true;            // PPS_EXT is connected
        bool ref40_present = true;        // the FPGA sees the 40 MHz reference
        int interval_error = 0;           // cycles added to every interval this board measures
        unsigned bad_interval_number = 0; // this board's Nth measured interval is 100 cycles off
        int lose_pulse_at_mcs_step = -1;  // no trigger pulse reaches or leaves this board in that MCS step
        bool time_load_blocked = false;   // the armed time load never happens
        int64_t time_load_offset = 0;     // added to the time that is loaded
        int reference_readback = -1;      // adf4002_status? reports this source, not the selected one
        std::string failing_command;      // this command is answered with "ERR injected"

        // --- Registers.
        unsigned reference_source = 0, ref_out = 0, trig_out = 0, trig_falling = 0, trig_width = 0, loopback = 0;
        uint32_t trig_period = 0;
        bool armed = false; // trigger armed for the next PPS edge
        uint32_t sync_count = 0, sync_interval = 0, pps_count = 0, pps_interval = 0, trig_count = 0;
        int mcs_step = -1;
        std::string time_source = "none";
        bool time_valid = false, time_armed = false;
        uint64_t time_pending = 0, last_pps = 0;

        // --- Model state.
        uint64_t next_pulse = 0, last_sync_at = 0, last_pps_at = 0;
        bool seen_sync = false, seen_pps = false;
        unsigned intervals_measured = 0;
    };

    // The boards and what connects them. chain: board 0's TRIG output feeds every wired board's
    // SYNC input. common: each board's TRIG feeds its own SYNC input.
    class SyncBus {
    public:
        enum class Wiring { chain, common };

        static constexpr uint64_t second = 40000000;

        explicit SyncBus(size_t n, Wiring w) : wiring(w), boards(n) {}

        Wiring wiring;
        uint64_t step = second / 20; // virtual time per status poll: 50 ms
        bool pps_running = true;     // the external PPS source

        // Registers and fault switches of one board. Change them only while no command is running.
        SyncBoard &board(size_t i) { return boards[i]; }

        // The SDK's reply to a sync, reference or time command of board i; nullopt for others.
        std::optional<std::string> command(size_t i, const std::string &text) {
            std::lock_guard<std::mutex> lock(mutex);
            auto &b = boards[i];
            if (!b.failing_command.empty() && text == b.failing_command) return "ERR injected";
            const auto value = [&](const char *name) -> std::optional<std::string> {
                const std::string prefix = std::string(name) + "=";
                if (text.rfind(prefix, 0) != 0) return std::nullopt;
                return text.substr(prefix.size());
            };
            if (text == "sync_status?") {
                advance(step);
                return status(b);
            }
            if (text == "time_status?") {
                advance(step);
                return time_status(b);
            }
            if (text == "adf4002_status?") return reference_status(b);
            if (auto v = value("clock_ref_source")) return set(b.reference_source, *v);
            if (auto v = value("ref_out")) return set(b.ref_out, *v);
            if (auto v = value("trig_out")) return set(b.trig_out, *v);
            if (auto v = value("trig_width")) return set(b.trig_width, *v);
            if (auto v = value("sync_loopback")) return set(b.loopback, *v);
            if (auto v = value("trig_edge")) {
                b.trig_falling = *v == "falling";
                return "";
            }
            if (auto v = value("trig_period")) {
                b.trig_period = static_cast<uint32_t>(std::stoul(*v));
                b.next_pulse = now + b.trig_period;
                return "";
            }
            if (auto v = value("mcs_step")) {
                b.mcs_step = std::stoi(*v);
                return "";
            }
            if (auto v = value("time_source")) {
                b.time_source = *v;
                return "";
            }
            if (auto v = value("time_next_pps")) {
                b.time_pending = std::stoull(*v);
                b.time_armed = true;
                b.time_valid = false;
                return "";
            }
            if (text == "trig_disarm!") {
                b.armed = false;
                return "";
            }
            if (text == "trig_at_pps!") {
                b.armed = true;
                return "";
            }
            if (text == "trig_pulse!") {
                fire(i, now);
                return "";
            }
            if (text == "sync_clear!") {
                b.sync_count = b.sync_interval = b.trig_count = 0;
                b.seen_sync = false;
                return "";
            }
            return std::nullopt;
        }

    private:
        static std::string set(unsigned &field, const std::string &text) {
            field = static_cast<unsigned>(std::stoul(text));
            return "";
        }

        static std::string number(uint64_t v) { return std::to_string(v); }

        static std::string status(const SyncBoard &b) {
            return "capable=1 version=10 ref40_present=" + number(b.ref40_present) +
                   " ref40_hz=40000000 ref_out=" + number(b.ref_out) + " trig_out=" + number(b.trig_out) +
                   " trig_edge=" + (b.trig_falling ? "falling" : "rising") + " trig_width=" + number(b.trig_width) +
                   " trig_period=" + number(b.trig_period) + " armed=" + number(b.armed) +
                   " loopback=" + number(b.loopback) + " time_pps=" + (b.time_source == "sync" ? "sync" : "ext") +
                   " sync_count=" + number(b.sync_count) + " sync_interval=" + number(b.sync_interval) +
                   " pps_count=" + number(b.pps_count) + " pps_interval=" + number(b.pps_interval) +
                   " trig_count=" + number(b.trig_count) + " mcs_step=" + std::to_string(b.mcs_step) +
                   " mcs_done=" + number(b.mcs_step == 5);
        }

        std::string time_status(const SyncBoard &b) const {
            return "valid=" + number(b.time_valid) + " armed=" + number(b.time_armed) + " source=" + b.time_source +
                   " ref40_present=" + number(b.ref40_present) + " pps_count=" + number(b.pps_count) +
                   " tick_rate=30720000 now=" + number(now) + " last_pps=" + number(b.last_pps) +
                   " framing=1 rx_timed=0 version=13";
        }

        static std::string reference_status(const SyncBoard &b) {
            const auto source = number(b.reference_readback >= 0 ? unsigned(b.reference_readback) : b.reference_source);
            return "source=" + source + " initialized=1 gpio_verified=1 chip_verified=1 lock_known=1 locked=1" +
                   " led_known=1 led_on=1 led_mode=" + source +
                   " reference_hz=10000000 oscillator_hz=40000000 r_counter=1 n_counter=4 source_known=1 last_error=0";
        }

        // What a board adds to an interval it measures.
        static uint32_t measured(SyncBoard &b, uint64_t cycles) {
            int64_t v = int64_t(cycles) + b.interval_error;
            if (++b.intervals_measured == b.bad_interval_number) v += 100;
            return static_cast<uint32_t>(v);
        }

        void load_time(SyncBoard &b, const char *source) {
            if (!b.time_armed || b.time_source != source || b.time_load_blocked) return;
            b.time_armed = false;
            b.time_valid = true;
            b.last_pps = uint64_t(int64_t(b.time_pending) + b.time_load_offset);
        }

        // A SYNC edge at one board.
        void sync_edge(SyncBoard &b, uint64_t at) {
            if (b.lose_pulse_at_mcs_step >= 0 && b.lose_pulse_at_mcs_step == b.mcs_step) return;
            if (b.seen_sync) b.sync_interval = measured(b, at - b.last_sync_at);
            b.seen_sync = true;
            b.last_sync_at = at;
            ++b.sync_count;
            load_time(b, "sync");
        }

        // Board i generates one TRIG pulse.
        void fire(size_t i, uint64_t at) {
            auto &source = boards[i];
            if (source.lose_pulse_at_mcs_step >= 0 && source.lose_pulse_at_mcs_step == source.mcs_step) return;
            ++source.trig_count;
            if (source.loopback) {
                sync_edge(source, at);
                return;
            }
            if (!source.trig_out) return;
            if (wiring == Wiring::common) {
                sync_edge(source, at);
                return;
            }
            if (i != 0) return; // only the master's TRIG is wired to the splitter
            for (auto &b: boards)
                if (b.sync_wired) sync_edge(b, at);
        }

        void pps_edge(uint64_t at) {
            for (size_t i = 0; i < boards.size(); ++i) {
                auto &b = boards[i];
                if (!b.pps_wired) continue;
                if (b.seen_pps) b.pps_interval = measured(b, at - b.last_pps_at);
                b.seen_pps = true;
                b.last_pps_at = at;
                ++b.pps_count;
                if (b.armed) {
                    b.armed = false;
                    fire(i, at);
                }
                load_time(b, "external");
            }
        }

        // Moves time forward, producing every periodic TRIG and PPS edge on the way, in order.
        void advance(uint64_t cycles) {
            const uint64_t until = now + cycles;
            for (;;) {
                uint64_t at = until + 1;
                int who = -2; // -1: PPS, otherwise the board whose periodic TRIG is due
                if (pps_running && next_pps <= until) {
                    at = next_pps;
                    who = -1;
                }
                for (size_t i = 0; i < boards.size(); ++i)
                    if (boards[i].trig_period && boards[i].next_pulse < at && boards[i].next_pulse <= until) {
                        at = boards[i].next_pulse;
                        who = static_cast<int>(i);
                    }
                if (who == -2) break;
                if (who == -1) {
                    pps_edge(at);
                    next_pps += second;
                } else {
                    fire(static_cast<size_t>(who), at);
                    boards[static_cast<size_t>(who)].next_pulse += boards[static_cast<size_t>(who)].trig_period;
                }
            }
            now = until;
        }

        std::mutex mutex;
        std::vector<SyncBoard> boards;
        uint64_t now = 0, next_pps = second;
    };

    // N devices on one SyncBus, ready for och::core::multi_sync.
    class SyncRig {
    public:
        static constexpr const char *system_info =
                "transport=framed16k firmware=FAKE_SDK_1.0 hw_time=1 sync=1 ref_source=1";

        explicit SyncRig(size_t n, SyncBus::Wiring wiring = SyncBus::Wiring::chain,
                         const std::string &capabilities = system_info)
            : bus(n, wiring) {
            for (size_t i = 0; i < n; ++i) {
                benches.push_back(std::make_unique<Bench>(capabilities));
                benches[i]->link->on_command = [this, i](const std::string &c) { return bus.command(i, c); };
            }
        }

        SyncBus bus;

        size_t size() const { return benches.size(); }

        SyncBoard &board(size_t i) { return bus.board(i); }

        Bench &bench(size_t i) { return *benches[i]; }

        // Commands board i has received since the previous call (the first call drops the handshake).
        std::vector<std::string> commands(size_t i) {
            const auto all = benches[i]->link->commands();
            if (seen.size() <= i) seen.resize(benches.size(), 3);
            std::vector<std::string> fresh(all.begin() + static_cast<std::ptrdiff_t>(seen[i]), all.end());
            seen[i] = all.size();
            return fresh;
        }

        static och_multi_sync_args args(uint32_t mode = OCH_SYNC_CHAIN) {
            och_multi_sync_args a{};
            a.struct_size = sizeof a;
            a.mode = mode;
            a.mcs = 1;
            a.lock_timeout_ms = 2000;
            a.start_time = 1000000;
            return a;
        }

        // Runs multi_sync on every board. Returns OCH_OK or the Failure code; what gets its text.
        int run(const och_multi_sync_args &a, std::vector<och_multi_sync_result> *results = nullptr,
                std::string *what = nullptr) {
            std::vector<Device *> devices;
            for (auto &b: benches) devices.push_back(&b->device);
            if (results) results->assign(benches.size(), och_multi_sync_result{});
            return code_of([&] { multi_sync(devices.data(), devices.size(), a, results ? results->data() : nullptr); },
                           what);
        }

    private:
        std::vector<std::unique_ptr<Bench>> benches;
        std::vector<size_t> seen;
    };
} // namespace och::test
