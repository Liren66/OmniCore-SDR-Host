#pragma once
#include <omnicore/och.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace och_cli::stream_assessment {
    // Qualification policy: recovered RX overflows observed within the first second.
    // All counters remain intact; this affects only the CLI's stability verdict.
    inline constexpr double startup_seconds = 1.0;
    // Stop only on repeated overflow onsets after the startup window. It watches
    // the RX device_overflows counter (GAP blocks seen in the data path), not the
    // RX_GAP events: in a TX underrun storm the FPGA event FIFO overflows and
    // drops events, while the GAP flags in the data blocks are never lost.
    struct repeated_overflow_observer {
        double previous = -1;
        uint64_t last = 0;
        bool repeated = false;
        void observe(uint64_t device_overflows, double seconds) {
            if (device_overflows <= last) return;
            last = device_overflows;
            if (!std::isfinite(seconds) || seconds <= startup_seconds) return;
            if (previous >= 0 && seconds >= previous && seconds - previous <= 2.0) repeated = true;
            previous = seconds;
        }
    };
    struct startup_observer {
        uint64_t gaps = 0;
        void observe(const och_event &e, double seconds) {
            if (!std::isfinite(seconds) || seconds < 0 || seconds > startup_seconds) return;
            if (e.type == OCH_EVENT_RX_GAP) gaps += e.count ? e.count : 1; // SDK summaries carry count
        }
        uint64_t accepted(const och_stats &rx) const { return std::min(gaps, rx.device_overflows); }
    };
    // resyncs counts corrupt framed headers; it is never excused.
    inline uint64_t remaining_faults(const och_stats &s, bool, uint64_t accepted) {
        return s.transfer_errors + s.host_dropped_samples + s.device_underflows + s.event_drops +
               s.tx_starvation_events + s.tx_padding_samples + s.resyncs +
               (s.device_overflows - std::min(s.device_overflows, accepted));
    }
    inline bool throughput_ok(const och_stats &s, unsigned channels, uint32_t rate, unsigned bytes) {
        const double expected = static_cast<double>(rate) * channels * bytes;
        return s.elapsed_ns > 0 && expected > 0 &&
               static_cast<double>(s.usb_bytes) / (s.elapsed_ns / 1e9) >= expected * 0.98;
    }
} // namespace och_cli::stream_assessment
