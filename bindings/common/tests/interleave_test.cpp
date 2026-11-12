// Offline test of channel (de)interleaving and RX block verdicts (Interleave.hpp, RxTracker.hpp).
#include "common/Interleave.hpp"
#include "common/RxTracker.hpp"
#include "common/TxBurst.hpp"
#include <algorithm>
#include <cstdio>
#include <vector>

using namespace omnicore_bindings;

static int errors = 0;
#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++errors;                                                                                                  \
        }                                                                                                              \
    } while (0)

// Byte b of sample k on channel c, so every byte of every sample is distinguishable.
static uint8_t pattern(size_t k, size_t c, size_t b) { return uint8_t(k * 16 + c * 8 + b); }

static void check_sizes(size_t bytes) {
    const size_t n = 37, offset = 5, channels = 2;
    std::vector<uint8_t> inter((n + offset) * channels * bytes);
    for (size_t k = 0; k < n + offset; ++k)
        for (size_t c = 0; c < channels; ++c)
            for (size_t b = 0; b < bytes; ++b) inter[(k * channels + c) * bytes + b] = pattern(k, c, b);

    // Misaligned destinations: SoapySDR buffers need not be aligned to the sample size.
    std::vector<uint8_t> a(n * bytes + 1), b(n * bytes + 1);
    void *dst[2] = {a.data() + 1, b.data() + 1};
    deinterleave(inter.data(), dst, channels, bytes, offset, n);
    bool ok = true;
    for (size_t k = 0; k < n; ++k)
        for (size_t i = 0; i < bytes; ++i)
            ok &= a[1 + k * bytes + i] == pattern(k + offset, 0, i) && b[1 + k * bytes + i] == pattern(k + offset, 1, i);
    EXPECT(ok);

    // And back.
    std::vector<uint8_t> again(n * channels * bytes);
    const void *src[2] = {a.data() + 1, b.data() + 1};
    interleave(src, again.data(), channels, bytes, n);
    EXPECT(std::equal(again.begin(), again.end(), inter.begin() + offset * channels * bytes));
}

static och_metadata meta(uint32_t flags, uint64_t time, uint64_t dropped = 0) {
    och_metadata m{};
    m.struct_size = sizeof m;
    m.flags = flags;
    m.time_ticks = time;
    m.host_dropped_samples = dropped;
    return m;
}

int main() {
    check_sizes(2);
    check_sizes(4);
    check_sizes(8);

    // Single channel is a plain copy with offset.
    {
        std::vector<uint32_t> in{1, 2, 3, 4, 5}, out(3);
        void *dst[1] = {out.data()};
        deinterleave(in.data(), dst, 1, 4, 2, 3);
        EXPECT(out[0] == 3 && out[2] == 5);
    }

    const uint32_t T = OCH_META_HAS_TIME;
    RxTracker t;
    // First block after start: reference, no error even though nothing came before.
    auto v = t.observe(meta(T | OCH_META_START, 1000), 100);
    EXPECT(!v.overflow && !v.late && v.has_time && v.time_ticks == 1000);
    // Continuous.
    v = t.observe(meta(T, 1100), 50);
    EXPECT(!v.overflow && v.time_ticks == 1100);
    // LO retune: RECONFIGURED|DISCONTINUITY but time continues -> no overflow.
    v = t.observe(meta(T | OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY, 1150), 10);
    EXPECT(!v.overflow);
    // Time jump (e.g. sample-rate change) -> overflow so gr-soapy re-tags rx_time.
    v = t.observe(meta(T | OCH_META_RECONFIGURED | OCH_META_START, 5000), 10);
    EXPECT(v.overflow && v.time_ticks == 5000);
    // Explicit overflow flags and dropped samples.
    v = t.observe(meta(T | OCH_META_DEVICE_OVERFLOW, 5010), 10);
    EXPECT(v.overflow);
    v = t.observe(meta(T, 5020, 7), 10);
    EXPECT(v.overflow);
    // Late timed command.
    v = t.observe(meta(T | OCH_META_LATE, 5030), 10);
    EXPECT(v.late && !v.overflow);
    // Partial block without time: extrapolated.
    v = t.observe(meta(0, 0), 10);
    EXPECT(!v.has_time && v.time_known && v.time_ticks == 5040);
    {
        RxTracker fresh;
        v = fresh.observe(meta(0, 0), 10);
        EXPECT(!v.time_known);
    }
    // After reset (new activation) a different time is not a jump.
    t.reset();
    v = t.observe(meta(T | OCH_META_START, 42), 10);
    EXPECT(!v.overflow && v.time_ticks == 42);

    // TX bursts.
    {
        const uint32_t SOB = OCH_TX_START_OF_BURST, EOB = OCH_TX_END_OF_BURST, HT = OCH_TX_HAS_TIME;
        TxBurst b;
        b.reset();
        EXPECT(b.flags(false, false) == SOB);           // first write starts a burst
        EXPECT(!b.starved_is_underflow());              // nothing sent yet
        EXPECT(!b.accepted(100, 100, false));
        EXPECT(b.flags(false, false) == 0);              // continuing
        EXPECT(b.starved_is_underflow());                // open burst running dry
        EXPECT(!b.should_start(1000) && b.queued() == 100);
        EXPECT(b.flags(false, true) == EOB);
        EXPECT(!b.accepted(40, 50, true));               // partial: EOB not applied, burst stays open
        EXPECT(b.starved_is_underflow());
        EXPECT(b.accepted(10, 10, true));                // rest with END_BURST closes it
        EXPECT(!b.starved_is_underflow());               // idle between bursts is not an underflow
        EXPECT(b.should_start(1000));                    // a closed burst starts the stream
        EXPECT(b.flags(true, true) == (SOB | HT | EOB)); // next: timed single-call burst
        EXPECT(b.flags(false, false) == SOB);
        b.reset();
        EXPECT(!b.should_start(1000) && b.queued() == 0);
        b.accepted(1000, 1000, false);
        EXPECT(b.should_start(1000));                    // one transfer queued
        EXPECT(b.flags(true, false) == (HT | SOB));      // a timed write always starts a burst
    }

    if (errors) std::printf("%d failure(s)\n", errors);
    else std::printf("soapy interleave: all checks passed\n");
    return errors ? 1 : 0;
}
