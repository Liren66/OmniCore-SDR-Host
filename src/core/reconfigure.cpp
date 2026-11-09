#include "internal.hpp"
using namespace och::core;

namespace {
    // Keeps application IO (recv / rx_acquire / send / flush) out of the streams during a
    // reconfiguration. Rule: while IoPause holds a stream's io_mutex, that stream's
    // `reconfiguring` is true. The flag is set before locking, so an IO call that is waiting
    // while holding io_mutex returns OCH_TIMEOUT and lets go; it is cleared before unlocking,
    // so in between a new IO call fails try_lock and also returns OCH_TIMEOUT.
    class IoPause {
    public:
        IoPause(Stream *r, Stream *t) : rx(r), tx(t) {
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
        IoPause(const IoPause &) = delete;
        IoPause &operator=(const IoPause &) = delete;

        // Let the TX producer refill the ring while the hardware restarts.
        void release_tx() {
            if (tx && tx_lock.owns_lock()) {
                tx->reconfiguring = false;
                tx_lock.unlock();
                tx->ring.wake();
            }
        }
        // Failure path: take TX back before cleaning up.
        void acquire_tx() {
            if (tx && !tx_lock.owns_lock()) {
                tx->reconfiguring = true;
                tx->ring.wake();
                tx_lock.lock();
            }
        }

    private:
        Stream *rx;
        Stream *tx;
        std::unique_lock<std::mutex> rx_lock, tx_lock;
    };
} // namespace

// Caller owns state_mutex. IO never takes state_mutex, so long recv/send waits
// can be interrupted before acquiring their gates without lock inversion.
void Device::reconfigure(const std::function<void()> &apply) {
    if (!rx && !tx) {
        apply();
        return;
    }
    IoPause pause(rx, tx);
    require(!rx || !rx->reader.lease, OCH_BUSY, "release RX lease before changing configuration");
    const ResumePlan r = rx ? rx->snapshot_for_reconfigure() : ResumePlan{};
    const ResumePlan t = tx ? tx->snapshot_for_reconfigure() : ResumePlan{};
    uint64_t r_discarded = 0, t_discarded = 0, t_inflight = 0;
    const auto began = now_ns();
    reconfiguring_control = true;
    try {
        command("usb_stop!");
        rx_on = tx_on = false;
        for (auto *s: {rx, tx})
            if (s) {
                const auto cancelled = s->halt_for_reconfigure();
                if (s == tx) t_inflight = cancelled;
            }
        transport_dirty = true;
        reset_transport();
        apply();
        command("usb_clear!");
        if (rx) r_discarded = rx->discard_for_reconfigure(r);
        if (tx) t_discarded = tx->discard_for_reconfigure(t);
        // FIR/clock helpers conservatively set transport_dirty; the transport
        // was already quiesced and cleaned in this transaction.
        transport_dirty = false;
        uint64_t zero_samples = 0;
        if (t.resume) zero_samples = tx->prefill_zero(4);
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
                     " rx_discarded_samples=" + std::to_string(r_discarded) + " tx_queued_discarded_samples=" +
                     std::to_string(t_discarded) + " tx_cancelled_bytes=" + std::to_string(t_inflight) +
                     " tx_delivery=unknown tx_padding_samples=" + std::to_string(zero_samples) +
                     " elapsed_us=" + std::to_string((now_ns() - began) / 1000),
             rx ? rx->rx_state.epoch_base : 0);
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
            if (s) s->fail_reconfigure();
        configured = false;
        transport_dirty = true;
        reconfiguring_control = false;
        emit(OCH_EVENT_TRANSPORT,
             "reconfigure failed; streams stopped; configure and recreate streams" +
                     (cleanup_error.empty() ? std::string() : "; hardware stop also failed: " + cleanup_error));
        std::rethrow_exception(original);
    }
}
