// What one RX call (receive / RxStream.read) reports next to its samples: the time of the first
// sample and where samples were lost (MEX_PLAN.md 3.2 meta). Pure logic on och_metadata values,
// tested offline in tests/rx_gaps_test.cpp.
//
// Unlike the Soapy plugin, nothing is held back: a call returns every sample it received in one
// array, and each loss is a row of Gaps = [index of the first sample after the loss (1-based),
// samples lost]. The count comes from the hardware time of the blocks (1 tick = 1 sample), so a
// loss between two reads, while MATLAB was busy elsewhere, shows at index 1 of the next read. A
// loss of unknown size (the time went backwards after a sample-rate change, or a block without
// time) has NaN as its count.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <omnicore/och.h>
#include <vector>

namespace omnicore_mex {
    struct RxGap {
        double index; // 1-based row of the first sample after the loss
        double lost;  // samples per channel, NaN if unknown
    };

    // Result of one call.
    struct RxReport {
        bool time_known = false;
        uint64_t time_ticks = 0; // first sample of the call
        uint32_t tick_rate = 0;  // from the blocks; 0 if none carried it
        bool late = false;         // a timed start was already past (OCH_META_LATE)
        bool reconfigured = false; // the device was reconfigured during the call
        bool end_of_burst = false; // last block of a finite RX command
        std::vector<RxGap> gaps;
        size_t blocks = 0;

        bool overflow() const { return !gaps.empty(); }
        double lost() const {
            double sum = 0;
            for (const auto &g: gaps) sum += g.lost;
            return sum;
        }
    };

    // Lives as long as an RX stream; reset() when it starts.
    class RxGapTracker {
    public:
        void reset() { seen_ = expected_valid_ = false; }

        // One block of n samples per channel, placed at 0-based row `row` of the call's output.
        void observe(const och_metadata &m, size_t n, size_t row, RxReport &r) {
            const bool has_time = (m.flags & OCH_META_HAS_TIME) != 0;
            const double unknown = std::numeric_limits<double>::quiet_NaN();
            const double index = double(row) + 1;

            if (has_time && expected_valid_) {
                if (m.time_ticks > expected_)
                    r.gaps.push_back({index, double(m.time_ticks - expected_)});
                else if (m.time_ticks < expected_)
                    r.gaps.push_back({index, unknown}); // time base restarted
            } else if (!has_time && seen_) {
                if (m.host_dropped_samples)
                    r.gaps.push_back({index, double(m.host_dropped_samples)});
                else if (m.flags & (OCH_META_HOST_OVERFLOW | OCH_META_DEVICE_OVERFLOW))
                    r.gaps.push_back({index, unknown});
            }

            if (r.blocks++ == 0) {
                r.time_known = has_time || expected_valid_;
                r.time_ticks = has_time ? m.time_ticks : expected_;
            }
            if (m.tick_rate && !r.tick_rate) r.tick_rate = m.tick_rate;
            if (m.flags & OCH_META_LATE) r.late = true;
            if (m.flags & OCH_META_RECONFIGURED) r.reconfigured = true;
            if (m.flags & OCH_META_END_OF_BURST) r.end_of_burst = true;

            seen_ = true;
            if (has_time) {
                expected_ = m.time_ticks;
                expected_valid_ = true;
            }
            if (expected_valid_) expected_ += n;
        }

    private:
        bool seen_ = false; // a block since reset(): later blocks can report a loss
        bool expected_valid_ = false;
        uint64_t expected_ = 0; // time of the next sample
    };
} // namespace omnicore_mex
