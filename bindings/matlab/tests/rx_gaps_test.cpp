// Offline test of the RX loss report (bindings/matlab/mex/RxGaps.hpp).
#include "RxGaps.hpp"
#include <cmath>
#include <cstdio>

using namespace omnicore_mex;

namespace {
    int failures = 0;
    void expect(bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }
    och_metadata block(uint64_t time, uint32_t flags = OCH_META_HAS_TIME) {
        och_metadata m{};
        m.struct_size = sizeof m;
        m.flags = flags;
        m.time_ticks = time;
        m.tick_rate = 30720000;
        return m;
    }
} // namespace

int main() {
    {
        // Contiguous blocks: no gap, time of the first sample.
        RxGapTracker t;
        RxReport r;
        t.observe(block(1000, OCH_META_HAS_TIME | OCH_META_START | OCH_META_DISCONTINUITY), 100, 0, r);
        t.observe(block(1100), 100, 100, r);
        t.observe(block(1200), 50, 200, r);
        expect(r.gaps.empty() && !r.overflow(), "contiguous blocks have no gap");
        expect(r.time_known && r.time_ticks == 1000, "time of the first block");
        expect(r.tick_rate == 30720000 && r.blocks == 3, "tick rate and block count");
    }
    {
        // A jump inside one call: the gap starts at the first row of the late block.
        RxGapTracker t;
        RxReport r;
        t.observe(block(0), 100, 0, r);
        t.observe(block(150), 100, 100, r);
        expect(r.gaps.size() == 1 && r.gaps[0].index == 101 && r.gaps[0].lost == 50, "gap inside a call");
        expect(r.overflow() && r.lost() == 50, "overflow and lost count");
    }
    {
        // A loss between two calls shows at index 1 of the second.
        RxGapTracker t;
        RxReport first, second;
        t.observe(block(0), 100, 0, first);
        t.observe(block(30720100), 100, 0, second);
        expect(first.gaps.empty(), "first call clean");
        expect(second.gaps.size() == 1 && second.gaps[0].index == 1 && second.gaps[0].lost == 30720000,
               "loss between calls at index 1");
        expect(second.time_ticks == 30720100, "second call time");
    }
    {
        // reset() (a new start) makes the next block a fresh reference.
        RxGapTracker t;
        RxReport a, b;
        t.observe(block(0), 100, 0, a);
        t.reset();
        t.observe(block(5000), 100, 0, b);
        expect(b.gaps.empty(), "no gap across a restart");
    }
    {
        // Time going backwards (sample rate change): a loss of unknown size; flags recorded.
        RxGapTracker t;
        RxReport r;
        t.observe(block(1000), 100, 0, r);
        t.observe(block(10, OCH_META_HAS_TIME | OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY), 100, 100, r);
        expect(r.gaps.size() == 1 && std::isnan(r.gaps[0].lost) && r.gaps[0].index == 101, "time reset gap");
        expect(r.reconfigured, "reconfigured flag");
    }
    {
        // A frequency change keeps time contiguous: reconfigured, but no gap.
        RxGapTracker t;
        RxReport r;
        t.observe(block(0), 100, 0, r);
        t.observe(block(100, OCH_META_HAS_TIME | OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY), 100, 100, r);
        expect(r.gaps.empty() && r.reconfigured, "retune without loss");
    }
    {
        // Blocks without time: host_dropped_samples counts, overflow flags without a count are NaN.
        RxGapTracker t;
        RxReport r;
        t.observe(block(0, 0), 100, 0, r);
        expect(!r.time_known, "no time on the first block");
        och_metadata m = block(0, 0);
        m.host_dropped_samples = 7;
        t.observe(m, 100, 100, r);
        t.observe(block(0, OCH_META_DEVICE_OVERFLOW), 100, 200, r);
        expect(r.gaps.size() == 2 && r.gaps[0].lost == 7 && r.gaps[0].index == 101, "host dropped count");
        expect(r.gaps.size() == 2 && std::isnan(r.gaps[1].lost) && r.gaps[1].index == 201, "overflow without count");
    }
    {
        // A timeless block after timed ones keeps extrapolating the time.
        RxGapTracker t;
        RxReport a, b;
        t.observe(block(500), 100, 0, a);
        t.observe(block(0, 0), 100, 0, b);
        expect(b.time_known && b.time_ticks == 600 && b.gaps.empty(), "extrapolated time");
    }
    {
        // Late start and end of burst.
        RxGapTracker t;
        RxReport r;
        t.observe(block(0, OCH_META_HAS_TIME | OCH_META_LATE), 100, 0, r);
        t.observe(block(100, OCH_META_HAS_TIME | OCH_META_END_OF_BURST), 10, 100, r);
        expect(r.late && r.end_of_burst, "late and end of burst");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("rx_gaps: all passed\n");
    return 0;
}
