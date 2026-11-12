// TX for MATLAB (MEX_PLAN.md 3.2, 3.3, 5.2, 5.3): one-shot transmit, the TxStream writes, and
// repeated transmission on a background thread.
//
// The och stream is created by the first write after construction or stop(), with the sample
// format of that write's input: single / double -> fc32, int16 -> sc16, int8 -> sc8. Later writes
// must use the same kind until stop(). Like the Soapy plugin, the stream is started (och_start)
// once one USB transfer is queued or a burst has been closed; a write without EndBurst keeps the
// burst open, the first write after an EndBurst (or with a StartTime) starts a new one.
//
// The repeat thread calls only och_* functions on its own copy of the samples; MATLAB may change
// or clear the input meanwhile.
#pragma once
#include "DeviceSession.hpp"
#include "TxEvents.hpp"
#include "common/TxBurst.hpp"
#include <atomic>
#include <cstdint>
#include <mex.h>
#include <omnicore/och.h>
#include <string>
#include <thread>
#include <vector>

namespace omnicore_mex {
    struct TxOptions {
        uint32_t buffer_bytes = 0; // host ring buffer; 0 = library default (64 MiB)
    };

    struct TxWrite {
        bool has_time = false;
        uint64_t time_ticks = 0;
        bool end_burst = false;
    };

    class TxSession {
    public:
        TxSession(DeviceSession &device, const TxOptions &options);
        ~TxSession();
        TxSession(const TxSession &) = delete;
        TxSession &operator=(const TxSession &) = delete;

        DeviceSession &device() const { return device_; }

        // Blocks until every sample is accepted. omnicore:TIMEOUT when no progress for `timeout` s.
        void write(const mxArray *x, const TxWrite &how, double timeout, Warnings &warnings);
        // Sends what is queued, stops and destroys the och stream. The next write creates a new one.
        void stop(Warnings &warnings);
        bool active() const { return stream_ != nullptr; }
        bool repeating() const { return repeat_.joinable(); }

        // Transmits x over and over until stop() (or the session is destroyed).
        void repeat(const mxArray *x, Warnings &warnings);

        // TX events since the last call (and the repeat thread's error, as an exception).
        mxArray *status();

        // One burst on a stream of its own; returns once the device acknowledged its end.
        static mxArray *transmit(DeviceSession &device, const TxOptions &options, const mxArray *x, const TxWrite &how,
                                 double timeout, Warnings &warnings);

    private:
        struct Input;
        Input inspect(const mxArray *x) const;
        void open(och_format format);
        void close(std::string *error, bool flush);
        // Rows [first, first + n) of the input -> interleaved samples at out. Returns clipped values.
        size_t prepare(const Input &in, size_t first, size_t n, uint8_t *out) const;
        void start_if_due();
        void stop_repeat();
        void repeat_loop(size_t samples);
        mxArray *status_struct(const TxStatus &status) const;

        DeviceSession &device_;
        TxOptions options_;
        och_stream *stream_ = nullptr;
        och_format format_ = OCH_FC32;
        size_t channels_ = 0, sample_bytes_ = 0, mtu_ = 0;
        uint32_t tick_rate_ = 0;
        bool started_ = false;
        omnicore_bindings::TxBurst burst_;
        std::vector<uint8_t> bounce_;
        TxStatus pending_; // events drained but not reported yet

        std::thread repeat_;
        std::atomic<bool> stop_repeat_{false};
        std::vector<uint8_t> repeat_samples_;
        std::string repeat_error_; // written by the thread before repeat_failed_
        std::atomic<bool> repeat_failed_{false};
    };
} // namespace omnicore_mex
