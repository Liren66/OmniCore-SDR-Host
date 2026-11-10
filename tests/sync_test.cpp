// Offline test of och::core::multi_sync on the SyncRig model (no device needed).
#include "sync_rig.hpp"
using namespace och::test;

namespace {
    size_t count_of(const std::vector<std::string> &all, const std::string &one) {
        return static_cast<size_t>(std::count(all.begin(), all.end(), one));
    }

    void test_chain() {
        std::puts("[1] CHAIN, two boards, with MCS");
        SyncRig rig(2);
        const auto a = SyncRig::args();
        std::vector<och_multi_sync_result> r;
        std::string why;
        CHECK(rig.run(a, &r, &why) == OCH_OK);
        if (!why.empty()) std::printf("  %s\n", why.c_str());
        for (size_t i = 0; i < 2; ++i) {
            CHECK(r[i].struct_size == sizeof r[i] && r[i].locked == 1 && r[i].edges >= 4);
            CHECK(r[i].interval == 4000000 && r[i].interval_error == 0);
            CHECK(r[i].mcs_done == 1 && r[i].time_valid == 1 && r[i].tick_rate == 30720000);
            CHECK(r[i].time_at_sync == a.start_time);
            const auto &b = rig.board(i);
            CHECK(b.ref_out == 1 && b.trig_width == 80 && b.mcs_step == 5 && b.time_source == "sync" && !b.armed);
            CHECK(b.time_valid && !b.time_armed && b.last_pps == a.start_time);
        }
        // Master: internal reference, the only TRIG output, the only one that generates pulses.
        CHECK(rig.board(0).reference_source == 0 && rig.board(1).reference_source == 1);
        CHECK(rig.board(0).trig_out == 1 && rig.board(1).trig_out == 0);
        CHECK(rig.board(0).trig_period == 0 && rig.board(1).trig_count == 0);
        const auto master = rig.commands(0), slave = rig.commands(1);
        CHECK(count_of(master, "trig_period=4000000") == 1 && count_of(slave, "trig_period=4000000") == 0);
        CHECK(count_of(master, "trig_pulse!") == 3 && count_of(slave, "trig_pulse!") == 0); // MCS 2, MCS 4, time load
        for (const auto &commands: {master, slave}) {
            for (int step = 0; step <= 5; ++step) CHECK(count_of(commands, "mcs_step=" + std::to_string(step)) == 1);
            CHECK(count_of(commands, "time_source=sync") == 1);
            CHECK(count_of(commands, "time_next_pps=" + std::to_string(a.start_time)) == 1);
        }
    }

    void test_common() {
        std::puts("[2] COMMON, two boards, with MCS");
        SyncRig rig(2, SyncBus::Wiring::common);
        rig.bus.step = SyncBus::second / 2;
        const auto a = SyncRig::args(OCH_SYNC_COMMON);
        std::vector<och_multi_sync_result> r;
        std::string why;
        CHECK(rig.run(a, &r, &why) == OCH_OK);
        if (!why.empty()) std::printf("  %s\n", why.c_str());
        for (size_t i = 0; i < 2; ++i) {
            CHECK(r[i].locked == 1 && r[i].interval == SyncBus::second && r[i].interval_error == 0);
            CHECK(r[i].mcs_done == 1 && r[i].time_valid == 1 && r[i].time_at_sync == a.start_time);
            const auto &b = rig.board(i);
            CHECK(b.reference_source == 1 && b.ref_out == 0 && b.trig_out == 1 && b.time_source == "external");
            CHECK(b.trig_count == 2 && b.sync_count == 2); // own TRIG at MCS steps 2 and 4
            const auto commands = rig.commands(i);
            CHECK(count_of(commands, "trig_at_pps!") == 2 && count_of(commands, "trig_pulse!") == 0);
        }
    }

    void test_unwired_slave() {
        std::puts("[3] CHAIN, the slave's SYNC input is not wired");
        SyncRig rig(2);
        rig.board(1).sync_wired = false;
        auto a = SyncRig::args();
        a.lock_timeout_ms = 300;
        std::vector<och_multi_sync_result> r;
        std::string why;
        CHECK(rig.run(a, &r, &why) == OCH_TIMEOUT);
        CHECK(why.find("board 1: no SYNC edges") != std::string::npos);
        CHECK(rig.board(0).trig_period == 0); // the lock-check generator is switched off again
        CHECK(rig.board(1).mcs_step == -1 && !rig.board(1).time_armed); // nothing after the lock check ran
        CHECK(r[0].struct_size == 0 && r[1].struct_size == 0);          // results are written on success only
    }

    // Runs a synchronisation that is expected to succeed and prints the reason if it does not.
    std::vector<och_multi_sync_result> must_pass(SyncRig &rig, const och_multi_sync_args &a) {
        std::vector<och_multi_sync_result> r;
        std::string why;
        CHECK(rig.run(a, &r, &why) == OCH_OK);
        if (!why.empty()) std::printf("  %s\n", why.c_str());
        return r;
    }

    void test_chain_variants() {
        std::puts("[4] CHAIN: single-board loopback, monitor period, external master reference, no MCS");
        {
            SyncRig rig(1);
            auto a = SyncRig::args();
            a.loopback = 1;
            const auto r = must_pass(rig, a);
            CHECK(r[0].locked && r[0].mcs_done && r[0].time_at_sync == a.start_time);
            CHECK(rig.board(0).loopback == 1 && rig.board(0).sync_count == rig.board(0).trig_count);
        }
        {
            SyncRig rig(2);
            auto a = SyncRig::args();
            a.monitor_period = 8000000;
            a.master_external_ref = 1;
            must_pass(rig, a);
            CHECK(rig.board(0).reference_source == 1 && rig.board(1).reference_source == 1);
            CHECK(rig.board(0).trig_period == 8000000 && rig.board(1).trig_period == 0);
            const auto master = rig.commands(0);
            CHECK(!master.empty() && master.back() == "trig_period=8000000");
        }
        {
            SyncRig rig(2);
            auto a = SyncRig::args();
            a.mcs = 0;
            const auto r = must_pass(rig, a);
            for (size_t i = 0; i < 2; ++i) {
                CHECK(r[i].locked && r[i].mcs_done == 0 && r[i].time_valid);
                CHECK(rig.board(i).mcs_step == -1);
                for (const auto &c: rig.commands(i)) CHECK(c.rfind("mcs_step=", 0) != 0);
            }
            CHECK(rig.board(0).trig_count >= 1); // lock-check pulses and the time-load pulse only
        }
    }

    void test_lock_tolerance() {
        std::puts("[5] lock check: one cycle of tolerance, and a bad interval restarts the count");
        {
            SyncRig rig(2);
            rig.board(0).interval_error = -1;
            rig.board(1).interval_error = 1;
            const auto r = must_pass(rig, SyncRig::args());
            CHECK(r[0].locked && r[0].interval_error == -1 && r[1].locked && r[1].interval_error == 1);
        }
        {
            // Intervals 1 and 2 are good, 3 is bad: three good ones in a row end with interval 6, at edge 7.
            SyncRig rig(2);
            rig.board(1).bad_interval_number = 3;
            const auto r = must_pass(rig, SyncRig::args());
            CHECK(r[1].locked && r[1].edges >= 7 && r[1].interval_error == 0);
        }
    }

    void test_arguments() {
        std::puts("[6] arguments and preconditions: rejected before any command is sent");
        SyncRig rig(2);
        Device *both[2] = {&rig.bench(0).device, &rig.bench(1).device};
        const auto call = [&](Device *const *devices, size_t n, const och_multi_sync_args &a,
                              std::string *what = nullptr) {
            return code_of([&] { multi_sync(devices, n, a, nullptr); }, what);
        };
        const auto a = SyncRig::args();
        CHECK(call(both, 0, a) == OCH_INVALID);
        CHECK(call(nullptr, 2, a) == OCH_INVALID);
        std::vector<Device *> many(65, both[0]);
        CHECK(call(many.data(), many.size(), a) == OCH_INVALID);
        Device *with_null[2] = {both[0], nullptr};
        CHECK(call(with_null, 2, a) == OCH_INVALID);
        Device *twice[2] = {both[0], both[0]};
        std::string why;
        CHECK(call(twice, 2, a, &why) == OCH_INVALID && why == "device listed twice");
        auto bad = a;
        bad.mode = 2;
        CHECK(call(both, 2, bad) == OCH_INVALID);
        bad = a;
        bad.loopback = 1;
        CHECK(call(both, 2, bad) == OCH_INVALID); // loopback is for one board
        bad.mode = OCH_SYNC_COMMON;
        CHECK(call(both, 1, bad) == OCH_INVALID); // and for CHAIN

        rig.bench(1).device.rx_on = true;
        CHECK(call(both, 2, a, &why) == OCH_BUSY && why.rfind("board 1: stop RX/TX streams", 0) == 0);
        rig.bench(1).device.rx_on = false;
        CHECK(rig.commands(0).empty() && rig.commands(1).empty());

        SyncRig no_sync(1, SyncBus::Wiring::chain, "transport=framed16k firmware=X hw_time=1 ref_source=1");
        CHECK(no_sync.run(a, nullptr, &why) == OCH_UNSUPPORTED && why.rfind("master: SDK capability sync=1", 0) == 0);
        SyncRig no_time(1, SyncBus::Wiring::chain, "transport=framed16k firmware=X sync=1 ref_source=1");
        CHECK(no_time.run(a, nullptr, &why) == OCH_UNSUPPORTED &&
              why.rfind("master: SDK capability hw_time=1", 0) == 0);
        CHECK(no_sync.commands(0).empty() && no_time.commands(0).empty());
    }

    void test_reference_failures() {
        std::puts("[7] step 1 failures: reference readback, missing 40 MHz, rejected command");
        const auto a = SyncRig::args();
        std::string why;
        {
            SyncRig rig(2);
            rig.board(1).reference_readback = 0; // asked for the external reference, still reports internal
            CHECK(rig.run(a, nullptr, &why) == OCH_PROTOCOL);
            CHECK(why == "reference source readback is unhealthy or mismatched"); // no board name (see plan, 9)
            CHECK(rig.board(0).ref_out == 1 && rig.board(0).trig_out == 1);       // the master was already set up
            CHECK(rig.board(1).ref_out == 0 && !rig.bench(1).device.configured);
        }
        {
            SyncRig rig(2);
            rig.board(1).ref40_present = false;
            CHECK(rig.run(a, nullptr, &why) == OCH_IO);
            CHECK(why == "40 MHz reference (CLK_FB_FPGA) not detected by the FPGA"); // no board name either
            CHECK(rig.board(1).reference_source == 1 && rig.board(1).ref_out == 0);
        }
        {
            SyncRig rig(2);
            rig.board(1).failing_command = "trig_disarm!";
            CHECK(rig.run(a, nullptr, &why) == OCH_PROTOCOL);
            CHECK(why.rfind("board 1: trig_disarm!: SDK rejected trig_disarm!", 0) == 0);
            CHECK(rig.board(1).ref_out == 1 && rig.board(1).trig_out == 0); // its outputs were set before the failure
            CHECK(rig.board(0).trig_period == 0 && rig.board(0).mcs_step == -1);
        }
    }

    void test_lock_failures() {
        std::puts("[8] lock check failures: reference off by five cycles, no PPS");
        std::string why;
        {
            SyncRig rig(2);
            rig.board(1).interval_error = 5;
            auto a = SyncRig::args();
            a.lock_timeout_ms = 400;
            std::vector<och_multi_sync_result> r;
            CHECK(rig.run(a, &r, &why) == OCH_TIMEOUT);
            CHECK(why.rfind("board 1: reference not locked, interval 4000005 cycles, expected 4000000", 0) == 0);
            CHECK(rig.board(0).trig_period == 0 && r[1].struct_size == 0);
        }
        {
            SyncRig rig(2, SyncBus::Wiring::common);
            rig.bus.step = SyncBus::second / 2;
            rig.bus.pps_running = false;
            auto a = SyncRig::args(OCH_SYNC_COMMON);
            a.lock_timeout_ms = 100; // COMMON adds three seconds
            CHECK(rig.run(a, nullptr, &why) == OCH_TIMEOUT);
            CHECK(why == "master: no PPS edges on PPS_EXT");
        }
    }

    void test_mcs_and_time_failures() {
        std::puts("[9] MCS pulse lost, time load missing, wrong time loaded");
        const auto a = SyncRig::args();
        std::string why;
        {
            SyncRig rig(2);
            rig.board(1).lose_pulse_at_mcs_step = 2;
            std::vector<och_multi_sync_result> r;
            CHECK(rig.run(a, &r, &why) == OCH_TIMEOUT);
            CHECK(why == "board 1: MCS step 2 pulse not seen (SYNC wiring?)");
            CHECK(rig.board(0).mcs_step == 5 && rig.board(1).mcs_step == 5); // both left in the final MCS state
            CHECK(rig.board(0).time_source == "none" && !rig.board(1).time_armed);
            CHECK(r[0].struct_size == 0);
        }
        {
            SyncRig rig(2);
            rig.board(1).time_load_blocked = true;
            CHECK(rig.run(a, nullptr, &why) == OCH_TIMEOUT);
            CHECK(why == "board 1: time load edge not seen");
            CHECK(rig.board(0).time_valid && rig.board(0).last_pps == a.start_time); // the master did load
            CHECK(rig.board(1).time_armed && !rig.board(1).time_valid && rig.board(1).time_source == "sync");
        }
        {
            SyncRig rig(2);
            rig.board(1).time_load_offset = 1;
            CHECK(rig.run(a, nullptr, &why) == OCH_PROTOCOL);
            CHECK(why == "board 1: time_last_pps after the load is not start_time");
        }
    }

    void test_late_monitor_check() {
        std::puts("[10] monitor_period below 1000 is reported after the boards are synchronised");
        SyncRig rig(2);
        auto a = SyncRig::args();
        a.monitor_period = 500;
        std::vector<och_multi_sync_result> r;
        std::string why;
        CHECK(rig.run(a, &r, &why) == OCH_INVALID && why == "monitor_period must be 0 or >= 1000 cycles");
        // Current behaviour (see plan, 9): everything before step 5 ran, and no results are returned.
        CHECK(rig.board(0).time_valid && rig.board(1).time_valid && rig.board(1).mcs_step == 5);
        CHECK(rig.board(0).trig_period == 0 && r[0].struct_size == 0);
    }

    void test_unplug_and_concurrency() {
        std::puts("[11] a board unplugged during the run; another thread using a board during the run");
        const auto a = SyncRig::args();
        std::string why;
        {
            SyncRig rig(2);
            std::thread unplug([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                rig.bench(1).link->unplug();
            });
            CHECK(rig.run(a, nullptr, &why) == OCH_IO && why.rfind("board 1: ", 0) == 0);
            unplug.join();
            // The master is untouched by the failure and still answers.
            std::lock_guard<std::mutex> lock(rig.bench(0).device.state_mutex);
            CHECK(read_sync_status(rig.bench(0).device).capable == 1);
        }
        {
            // multi_sync takes each device's state_mutex per transaction, so other control calls interleave.
            SyncRig rig(2);
            std::atomic<bool> done{false};
            std::atomic<unsigned> queries{0};
            std::thread other([&] {
                auto &d = rig.bench(1).device;
                while (!done) {
                    // A polling client. Without the pause this loop keeps state_mutex almost
                    // all the time and starves multi_sync into its lock timeout.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    std::lock_guard<std::mutex> lock(d.state_mutex);
                    if (!d.command("adf4002_status?").empty()) ++queries; // does not advance the model's time
                }
            });
            const auto r = must_pass(rig, a);
            done = true;
            other.join();
            CHECK(r[0].locked && r[1].locked && r[1].time_at_sync == a.start_time && queries > 10);
        }
    }
} // namespace

int main() {
    test_chain();
    test_common();
    test_unwired_slave();
    test_chain_variants();
    test_lock_tolerance();
    test_arguments();
    test_reference_failures();
    test_lock_failures();
    test_mcs_and_time_failures();
    test_late_monitor_check();
    test_unplug_and_concurrency();
    std::printf("SYNC TEST: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
