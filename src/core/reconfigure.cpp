#include "internal.hpp"
using namespace och::core;

namespace {
    struct IoPause {
        och_stream *rx;
        och_stream *tx;
        std::unique_lock<std::mutex> rx_lock, tx_lock;
        IoPause(och_stream *r, och_stream *t) : rx(r), tx(t) {
            for (auto *s: {rx, tx})
                if (s) {
                    s->reconfiguring = true;
                    s->ring.wake();
                }
            if (rx) rx_lock = std::unique_lock<std::mutex>(rx->io_mutex);
            if (tx) tx_lock = std::unique_lock<std::mutex>(tx->io_mutex);
        }
        ~IoPause() {
            for (auto *s: {rx, tx})
                if (s) {
                    s->reconfiguring = false;
                    s->ring.wake();
                }
        }
        void release_tx() {
            if (tx && tx_lock.owns_lock()) {
                tx->reconfiguring = false;
                tx_lock.unlock();
                tx->ring.wake();
            }
        }
        void acquire_tx() {
            if (tx && !tx_lock.owns_lock()) {
                tx->reconfiguring = true;
                tx->ring.wake();
                tx_lock.lock();
            }
        }
    };
    struct Session {
        och_stream *stream;
        bool resume, stopped, ever_started;
        uint64_t discarded = 0, inflight = 0;
        explicit Session(och_stream *s)
            : stream(s), resume(s && s->started && s->running && !s->stop_requested), stopped(s && s->stop_requested),
              ever_started(s && s->ever_started) {}
        void prepare() {
            if (!stream) return;
            auto &s = *stream;
            if (s.args.direction == OCH_RX) {
                const auto bytes = s.ring.queued() - s.recv_offset + s.rx_filled;
                discarded = bytes / s.group;
                s.host_dropped += discarded;
                s.dropped_pending += discarded;
            } else {
                // tx_offset is already copied out of the first ring block. Pending
                // USB transfers are uncertain delivery, not replayable samples.
                discarded = (s.ring.queued() - s.tx_offset + s.assembling.load()) / s.group;
            }
            s.prepare_restart(resume || ever_started);
            if (resume) {
                std::lock_guard<std::mutex> lock(s.failure_mutex);
                s.failure.clear();
            }
            if (stopped && !resume) {
                s.stop_requested = true;
                s.ever_started = ever_started;
                SetEvent(s.stop_event.h);
            }
        }
    };
}

// Caller owns state_mutex. IO never takes state_mutex, so long recv/send waits
// can be interrupted before acquiring their gates without lock inversion.
void och_device::reconfigure(const std::function<void()> &apply) {
    if (!rx && !tx) {
        apply();
        return;
    }
    IoPause pause(rx, tx);
    require(!rx || !rx->lease, OCH_BUSY, "release RX lease before changing configuration");
    Session r(rx), t(tx);
    const auto began = now_ns();
    reconfiguring_control = true;
    try {
        command("usb_stop!");
        rx_on = tx_on = false;
        for (auto *s: {rx, tx})
            if (s) {
                const auto before = s->cancelled_bytes.load();
                s->halt();
                if (s == tx) t.inflight = s->cancelled_bytes.load() - before;
            }
        transport_dirty = true;
        reset_transport();
        apply();
        command("usb_clear!");
        r.prepare();
        t.prepare();
        // FIR/clock helpers conservatively set transport_dirty; the transport
        // was already quiesced and cleaned in this transaction.
        transport_dirty = false;
        uint64_t zero_samples = 0;
        if (t.resume) {
            for (unsigned i = 0; i < 4; ++i) {
                auto *b = tx->ring.write();
                require(b != nullptr, OCH_IO, "TX restart prefill capacity");
                memset(b->data.get(), 0, tx->args.transfer_bytes);
                tx->ring.publish(tx->args.transfer_bytes, {});
            }
            zero_samples = 4ULL * tx->args.transfer_bytes / tx->group;
            tx->padding += zero_samples;
        }
        // The new configuration and zero prefill are committed. Let an
        // existing producer replenish the ring during hardware startup, rather
        // than draining the prefill while it is still locked out. RX starts
        // first so no further RX control round-trip delays the TX producer.
        if (t.resume) pause.release_tx();
        if (r.resume) rx->start();
        if (t.resume) tx->start();
        reconfiguring_control = false;
        emit(OCH_EVENT_RECONFIGURED,
             "reconfigure complete rx_resumed=" + std::to_string(r.resume) + " tx_resumed=" + std::to_string(t.resume) +
                     " rx_discarded_samples=" + std::to_string(r.discarded) + " tx_queued_discarded_samples=" +
                     std::to_string(t.discarded) + " tx_cancelled_bytes=" + std::to_string(t.inflight) +
                     " tx_delivery=unknown tx_padding_samples=" + std::to_string(zero_samples) +
                     " elapsed_us=" + std::to_string((now_ns() - began) / 1000),
             rx ? rx->epoch_base : 0);
    } catch (...) {
        const auto original = std::current_exception();
        // A failed update or restart never restarts TX with uncertain settings.
        std::string cleanup_error;
        pause.acquire_tx();
        try {
            command("usb_stop!");
            rx_on = tx_on = false;
        } catch (const std::exception &e) {
            cleanup_error = e.what();
        }
        for (auto *s: {rx, tx})
            if (s) {
                s->halt();
                // No stale data may look like a successful capture after failure.
                if (s == rx) {
                    const auto lost = (s->ring.queued() - s->recv_offset + s->rx_filled) / s->group;
                    s->host_dropped += lost;
                    s->dropped_pending += lost;
                }
                s->ring.reset();
                s->recv_offset = s->rx_filled = s->tx_offset = 0;
                s->assembling = 0;
                std::lock_guard<std::mutex> lock(s->failure_mutex);
                s->failure = "reconfiguration failed; configure and recreate streams";
            }
        configured = false;
        transport_dirty = true;
        reconfiguring_control = false;
        emit(OCH_EVENT_TRANSPORT,
             "reconfigure failed; streams stopped; configure and recreate streams" +
                     (cleanup_error.empty() ? std::string() : "; hardware stop also failed: " + cleanup_error));
        std::rethrow_exception(original);
    }
}
