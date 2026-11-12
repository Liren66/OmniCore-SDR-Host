// RX for MATLAB (MEX_PLAN.md 3.2, 3.3, 5.1): one-shot receive(N) and the continuous RxStream.
//
// An och stream starts only once, so an RxSession records the options and every start() creates
// and starts a fresh och_stream; stop() stops and destroys it (like the Soapy plugin's
// activateStream). The device has one RX stream slot: a second RX stream, or receive() while an
// RxStream runs, is refused by the core with BUSY.
//
// Samples land in a preallocated N x channels complex MATLAB array, one column per enabled RX
// channel. One channel in single / int16 / int8 is received straight into the array; two channels
// (and double) go through a bounce buffer and are split into the columns.
#pragma once
#include "DeviceSession.hpp"
#include "RxGaps.hpp"
#include <cstdint>
#include <mex.h>
#include <omnicore/och.h>
#include <string>
#include <vector>

namespace omnicore_mex {
    enum class SampleType { Single, Double, Int16, Int8 };
    SampleType sample_type(const std::string &name); // OutputDataType
    const char *sample_type_name(SampleType);

    class RxOutput;

    struct RxOptions {
        SampleType type = SampleType::Single;
        uint32_t buffer_bytes = 0; // host ring buffer; 0 = library default (64 MiB)
    };

    class RxSession {
    public:
        RxSession(DeviceSession &device, const RxOptions &options);
        ~RxSession();
        RxSession(const RxSession &) = delete;
        RxSession &operator=(const RxSession &) = delete;

        DeviceSession &device() const { return device_; }

        const RxOptions &options() const { return options_; }
        bool running() const { return stream_ != nullptr; }
        // Starts now, or with has_time at hardware time `ticks` (a timed RX command).
        void start(bool has_time = false, uint64_t ticks = 0);
        void stop();

        // Up to n samples per channel, waiting at most `timeout` seconds for each block. Returns
        // fewer rows when the wait runs out after some samples; omnicore:TIMEOUT when none came.
        // meta is built only when wanted (non-null).
        void read(size_t n, double timeout, mxArray **samples, mxArray **meta);

        // One finite RX command of n samples per channel on a stream of its own, now or (has_time)
        // at hardware time `ticks`.
        static void receive(DeviceSession &device, const RxOptions &options, size_t n, double timeout, bool has_time,
                            uint64_t ticks, mxArray **samples, mxArray **meta);

    private:
        void open(uint32_t start_flags, uint32_t num_samples, uint64_t start_ticks);
        void close(std::string *error);
        // Fills the rows of `out` until it is full, the burst ends or a block takes longer than the
        // wait (first_wait_ms for the first block of the call).
        size_t pump(RxOutput &out, uint32_t first_wait_ms, uint32_t wait_ms, RxReport &report, bool &timed_out);
        mxArray *meta_struct(const RxReport &report) const;

        DeviceSession &device_;
        RxOptions options_;
        och_stream *stream_ = nullptr;
        size_t channels_ = 0;
        uint32_t sample_rate_ = 0; // when the stream started; fallback tick rate
        RxGapTracker tracker_;
        std::vector<uint8_t> bounce_;
    };
} // namespace omnicore_mex
