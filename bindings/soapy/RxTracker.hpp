// Decides what readStream reports for each block och_recv returns (SOAPY_PLAN.md 4.2).
// Pure logic on och_metadata values; tested offline in tests/interleave_test.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <omnicore/och.h>

namespace omnicore_soapy {
    struct RxVerdict {
        bool overflow = false; // samples were lost before this block: report SOAPY_SDR_OVERFLOW first
        bool late = false;     // a timed RX command started late: report SOAPY_SDR_TIME_ERROR first
        bool has_time = false;   // time_ticks came from the hardware block header
        bool time_known = false; // has_time, or extrapolated from the previous block
        uint64_t time_ticks = 0; // time of the block's first sample
    };

    class RxTracker {
    public:
        // Call after every activation: the first block's time is a new reference, not a jump.
        void reset() { expected_valid_ = false; }

        RxVerdict observe(const och_metadata &m, size_t samples) {
            RxVerdict v;
            v.has_time = (m.flags & OCH_META_HAS_TIME) != 0;
            v.late = (m.flags & OCH_META_LATE) != 0;
            v.overflow = (m.flags & (OCH_META_HOST_OVERFLOW | OCH_META_DEVICE_OVERFLOW)) != 0 ||
                         m.host_dropped_samples > 0;
            if (v.has_time) {
                // gr-soapy re-tags rx_time only after an OVERFLOW return, so any jump in sample
                // time (sample-rate change, reconfiguration pause) must be reported as one.
                if (expected_valid_ && m.time_ticks != expected_) v.overflow = true;
                v.time_ticks = m.time_ticks;
                v.time_known = true;
                expected_ = m.time_ticks + samples;
                expected_valid_ = true;
            } else if (expected_valid_) {
                v.time_ticks = expected_;
                v.time_known = true;
                expected_ += samples;
            }
            return v;
        }

    private:
        bool expected_valid_ = false;
        uint64_t expected_ = 0;
    };
} // namespace omnicore_soapy
