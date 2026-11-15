// Offline test of the TX event queue (bindings/common/TxEvents.hpp).
#include "common/TxEvents.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

using namespace omnicore_bindings;

namespace {
    int failures = 0;
    void expect(bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }
    och_event event(uint32_t type, uint64_t time = 0, uint32_t count = 1) {
        och_event e{};
        e.struct_size = sizeof e;
        e.type = type;
        e.hw_time = time;
        e.count = count;
        std::snprintf(e.message, sizeof e.message, "event %u", type);
        return e;
    }
} // namespace

int main() {
    {
        // Nothing is queued until a session listens; non-TX events are ignored.
        TxEvents q;
        q.push(event(OCH_EVENT_TX_UNDERFLOW));
        q.listen(true);
        q.push(event(OCH_EVENT_RX_GAP));
        TxStatus s;
        q.drain(s);
        expect(s.underflows == 0 && s.acks == 0 && s.errors == 0, "not listening / not TX");
    }
    {
        // Starvation is an underflow only inside an open burst.
        TxEvents q;
        q.listen(true);
        q.push(event(OCH_EVENT_TX_STARVED));
        q.set_burst_open(true);
        q.push(event(OCH_EVENT_TX_STARVED, 0, 3));
        q.push(event(OCH_EVENT_TX_UNDERFLOW, 77));
        q.push(event(OCH_EVENT_TX_LATE, 88));
        q.push(event(OCH_EVENT_TX_BURST_ACK, 1000));
        q.push(event(OCH_EVENT_TX_ERROR));
        TxStatus s;
        q.drain(s);
        expect(s.starved == 3 && s.underflows == 1, "starved only inside a burst; underflow separate");
        expect(s.late == 1 && s.acks == 1 && s.errors == 1, "late, ack, error");
        expect(s.ack_ticks.size() == 1 && s.ack_ticks[0] == 1000, "ack time");
        expect(s.last_error == "event 10", "error message");
        TxStatus again;
        q.drain(again);
        expect(again.acks == 0 && again.underflows == 0, "drain empties the queue");
    }
    {
        // Bounded: the oldest are dropped and counted.
        TxEvents q;
        q.listen(true);
        for (size_t i = 0; i < TxEvents::capacity + 5; ++i) q.push(event(OCH_EVENT_TX_BURST_ACK, i));
        TxStatus s;
        q.drain(s);
        expect(s.acks == TxEvents::capacity && s.dropped == 5 && s.ack_ticks.front() == 5, "bounded queue");
        q.listen(false);
        q.push(event(OCH_EVENT_TX_BURST_ACK));
        q.listen(true);
        TxStatus t;
        q.drain(t);
        expect(t.acks == 0, "listen() starts empty");
    }
    {
        // wait_for returns when a wanted type arrives from another thread, keeping earlier events.
        TxEvents q;
        q.listen(true);
        q.set_burst_open(true);
        std::thread producer([&] {
            q.push(event(OCH_EVENT_TX_STARVED));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            q.push(event(OCH_EVENT_TX_BURST_ACK, 5));
        });
        TxStatus s;
        const auto start = std::chrono::steady_clock::now();
        const bool got = q.wait_for(TxEvents::bit(OCH_EVENT_TX_BURST_ACK), start + std::chrono::seconds(2), s);
        producer.join();
        expect(got && s.acks == 1 && s.starved == 1, "wait_for ack");
        TxStatus none;
        const bool timed_out = !q.wait_for(TxEvents::bit(OCH_EVENT_TX_BURST_ACK),
                                           std::chrono::steady_clock::now() + std::chrono::milliseconds(30), none);
        expect(timed_out, "wait_for times out");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("tx_events: all passed\n");
    return 0;
}
