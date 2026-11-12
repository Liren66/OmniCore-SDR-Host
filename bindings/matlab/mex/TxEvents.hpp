// TX events from the device event thread to the MATLAB calls (MEX_PLAN.md 5.4). The event thread
// pushes; transmit / TxStream.status drain. Pure C++ (no MATLAB), tested offline in
// tests/tx_events_test.cpp.
//
// The core emits TX_STARVED whenever its host TX queue runs empty with no USB transfer pending,
// including the normal idle time after a burst ended. Starvation outside an open burst is dropped
// (the TX session tells the queue whether a burst is open); inside one it is reported as Starved.
// It is not an RF underflow: the samples of a timed burst can be waiting in the FPGA for their
// start while the host queue is empty. Only the device's TX_UNDERFLOW counts as Underflow.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <omnicore/och.h>
#include <string>
#include <vector>

namespace omnicore_mex {
    struct TxEvent {
        uint32_t type;    // OCH_EVENT_TX_*
        uint64_t hw_time; // sample ticks, 0 if none
        uint32_t count;   // device events this one stands for
        std::string message;
    };

    // Counts since the last report.
    struct TxStatus {
        uint64_t underflows = 0, starved = 0, late = 0, acks = 0, errors = 0, dropped = 0;
        std::vector<uint64_t> ack_ticks;
        std::string last_error;

        void merge(const TxStatus &o) {
            underflows += o.underflows;
            starved += o.starved;
            late += o.late;
            acks += o.acks;
            errors += o.errors;
            dropped += o.dropped;
            ack_ticks.insert(ack_ticks.end(), o.ack_ticks.begin(), o.ack_ticks.end());
            if (!o.last_error.empty()) last_error = o.last_error;
        }

        void add(const TxEvent &e) {
            switch (e.type) {
            case OCH_EVENT_TX_BURST_ACK:
                acks += e.count;
                ack_ticks.push_back(e.hw_time);
                break;
            case OCH_EVENT_TX_UNDERFLOW:
                underflows += e.count;
                break;
            case OCH_EVENT_TX_STARVED:
                starved += e.count;
                break;
            case OCH_EVENT_TX_LATE:
                late += e.count;
                break;
            default:
                errors += e.count;
                last_error = e.message;
                break;
            }
        }
    };

    class TxEvents {
    public:
        static bool is_tx(uint32_t type) {
            return type == OCH_EVENT_TX_STARVED || type == OCH_EVENT_TX_BURST_ACK || type == OCH_EVENT_TX_LATE ||
                   type == OCH_EVENT_TX_UNDERFLOW || type == OCH_EVENT_TX_ERROR;
        }

        // A TX session starts or ends listening; either way the queue starts empty.
        void listen(bool on) {
            std::lock_guard<std::mutex> lock(mutex_);
            listening_ = on;
            queue_.clear();
            dropped_ = 0;
            burst_open_ = false;
        }
        void set_burst_open(bool open) { burst_open_ = open; }

        // Event thread.
        void push(const och_event &e) {
            if (!is_tx(e.type)) return;
            TxEvent t{e.type, e.hw_time, e.count ? e.count : 1, e.message};
            if (t.type == OCH_EVENT_TX_STARVED && !burst_open_) return; // idle between bursts
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!listening_) return;
                if (queue_.size() >= capacity) {
                    queue_.pop_front();
                    ++dropped_;
                }
                queue_.push_back(std::move(t));
            }
            cv_.notify_all();
        }

        // Adds every queued event to `status`.
        void drain(TxStatus &status) {
            std::lock_guard<std::mutex> lock(mutex_);
            take(status);
        }

        static constexpr uint32_t bit(uint32_t type) { return 1u << type; }

        // Drains into `status` until an event whose type is in `types` (a set of bit(type)) arrived
        // or the deadline passed. True if one did.
        bool wait_for(uint32_t types, std::chrono::steady_clock::time_point deadline, TxStatus &status) {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                bool found = false;
                for (const auto &e: queue_) found = found || (bit(e.type) & types) != 0;
                take(status);
                if (found) return true;
                if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                    for (const auto &e: queue_) found = found || (bit(e.type) & types) != 0;
                    take(status);
                    return found;
                }
            }
        }

        static constexpr size_t capacity = 1024;

    private:
        void take(TxStatus &status) {
            for (const auto &e: queue_) status.add(e);
            status.dropped += dropped_;
            queue_.clear();
            dropped_ = 0;
        }

        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<TxEvent> queue_;
        uint64_t dropped_ = 0;
        bool listening_ = false;
        std::atomic<bool> burst_open_{false};
    };
} // namespace omnicore_mex
