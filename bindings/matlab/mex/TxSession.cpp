#include "TxSession.hpp"
#include "MxConvert.hpp"
#include "TxConvert.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace omnicore_mex;
using clock_type = std::chrono::steady_clock;

struct TxSession::Input {
    const void *data = nullptr;
    bool complex = false;
    mxClassID cls = mxUNKNOWN_CLASS;
    size_t rows = 0;
    och_format format = OCH_FC32;
};

namespace {
    size_t channel_count(uint32_t mask) { return size_t((mask & 1) != 0) + size_t((mask & 2) != 0); }

    const void *data_of(const mxArray *x) {
        const bool c = mxIsComplex(x);
        switch (mxGetClassID(x)) {
        case mxSINGLE_CLASS:
            return c ? static_cast<const void *>(mxGetComplexSingles(x)) : mxGetSingles(x);
        case mxDOUBLE_CLASS:
            return c ? static_cast<const void *>(mxGetComplexDoubles(x)) : mxGetDoubles(x);
        case mxINT16_CLASS:
            return c ? static_cast<const void *>(mxGetComplexInt16s(x)) : mxGetInt16s(x);
        case mxINT8_CLASS:
            return c ? static_cast<const void *>(mxGetComplexInt8s(x)) : mxGetInt8s(x);
        default:
            return nullptr;
        }
    }

    std::string count(size_t n) { return std::to_string(n); }
} // namespace

TxSession::TxSession(DeviceSession &device, const TxOptions &options) : device_(device), options_(options) {}

TxSession::~TxSession() {
    stop_repeat();
    close(nullptr, false);
}

TxSession::Input TxSession::inspect(const mxArray *x) const {
    if (!x || !mxIsNumeric(x) || mxIsSparse(x) || mxGetNumberOfDimensions(x) > 2)
        throw invalid_argument("TX samples must be a numeric matrix, one column per TX channel");
    Input in;
    in.cls = mxGetClassID(x);
    switch (in.cls) {
    case mxSINGLE_CLASS:
    case mxDOUBLE_CLASS:
        in.format = OCH_FC32;
        break;
    case mxINT16_CLASS:
        in.format = OCH_SC16;
        break;
    case mxINT8_CLASS:
        in.format = OCH_SC8;
        break;
    default:
        throw invalid_argument("TX samples must be single, double, int16 or int8");
    }
    const och_config c = device_.config();
    const size_t channels = channel_count(c.tx_mask);
    if (!channels) throw invalid_argument("TxChannels is empty: enable a TX channel first");
    if (in.format == OCH_SC8 && c.usb_iq_bits != 8) throw invalid_argument("int8 TX samples need IQBits 8");
    const size_t m = mxGetM(x), n = mxGetN(x);
    if (channels == 1 && m == 1)
        in.rows = n; // a row vector is one channel too
    else if (n == channels || mxIsEmpty(x))
        in.rows = mxIsEmpty(x) ? 0 : m;
    else
        throw invalid_argument("TX samples need " + count(channels) + " column(s), one per TX channel; got " +
                               count(n));
    in.complex = mxIsComplex(x);
    in.data = data_of(x);
    return in;
}

size_t TxSession::prepare(const Input &in, size_t first, size_t n, uint8_t *out) const {
    switch (in.cls) {
    case mxSINGLE_CLASS:
        return gather(static_cast<const float *>(in.data), in.complex, in.rows, channels_, first, n,
                      reinterpret_cast<float *>(out));
    case mxDOUBLE_CLASS:
        return gather(static_cast<const double *>(in.data), in.complex, in.rows, channels_, first, n,
                      reinterpret_cast<float *>(out));
    case mxINT16_CLASS:
        return gather(static_cast<const int16_t *>(in.data), in.complex, in.rows, channels_, first, n,
                      reinterpret_cast<int16_t *>(out));
    default:
        return gather(static_cast<const int8_t *>(in.data), in.complex, in.rows, channels_, first, n,
                      reinterpret_cast<int8_t *>(out));
    }
}

void TxSession::open(och_format format) {
    const och_config c = device_.config();
    channels_ = channel_count(c.tx_mask);
    och_stream_args a = och_struct<och_stream_args>();
    och_default_stream_args(&a, OCH_TX);
    a.format = format;
    if (options_.buffer_bytes) a.host_buffer_bytes = options_.buffer_bytes;
    check(och_create_stream(device_.handle(), &a, &stream_), "create TX stream");
    format_ = format;
    sample_bytes_ = och_sample_size(format);
    // Samples per channel in one USB transfer: each 16 KiB block carries 16368 payload bytes.
    const size_t wire_bytes = c.usb_iq_bits == 8 ? 2 : 3;
    mtu_ = std::max<size_t>(1, a.transfer_bytes / 16384 * 16368 / (wire_bytes * channels_));
    bounce_.resize(mtu_ * channels_ * sample_bytes_);
    tick_rate_ = c.sample_rate;
    started_ = false;
    burst_.reset();
    pending_ = TxStatus();
    device_.tx_events().listen(true);
}

void TxSession::close(std::string *error, bool flush) {
    if (!stream_) return;
    if (started_ && flush && och_flush(stream_, 1000) != OCH_OK && error) *error = och_last_error();
    if (och_stop(stream_, 3000) != OCH_OK && error && error->empty()) *error = och_last_error();
    if (och_destroy_stream(stream_) != OCH_OK && error && error->empty()) *error = och_last_error();
    stream_ = nullptr;
    device_.tx_events().drain(pending_);
    device_.tx_events().listen(false);
}

void TxSession::start_if_due() {
    if (started_ || !burst_.should_start(mtu_)) return;
    // Blocks until the FPGA holds about one block (core TX prefill).
    check(och_start(stream_), "start TX");
    started_ = true;
}

void TxSession::write(const mxArray *x, const TxWrite &how, double timeout, Warnings &warnings) {
    if (repeating()) throw Error("omnicore:BUSY", "the device is transmitting repeatedly; stop that first");
    const uint32_t wait_ms = timeout_ms(timeout);
    const Input in = inspect(x);
    if (!stream_)
        open(in.format);
    else if (in.format != format_)
        throw invalid_argument("this TX stream carries " +
                               std::string(format_ == OCH_FC32 ? "single/double" : format_ == OCH_SC16 ? "int16" : "int8") +
                               " samples; stop it to change the type");
    const bool empty = in.rows == 0;
    if (empty && !how.end_burst) return;
    // A zero-length EndBurst cannot mark a block in the core: it ends the burst with one zero sample.
    const size_t total = empty ? 1 : in.rows;
    size_t done = 0, clipped = 0;
    bool first = true;
    while (done < total) {
        const size_t n = std::min(mtu_, total - done);
        if (empty)
            std::memset(bounce_.data(), 0, channels_ * sample_bytes_);
        else
            clipped += prepare(in, done, n, bounce_.data());
        const bool closes = how.end_burst && done + n == total;
        size_t offset = 0;
        auto deadline = clock_type::now() + std::chrono::milliseconds(wait_ms);
        while (offset < n) {
            auto md = och_struct<och_tx_metadata>();
            md.flags = burst_.flags(first && how.has_time, closes);
            md.time_ticks = how.time_ticks;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_type::now());
            size_t accepted = 0;
            const int rc = och_send_burst(stream_, bounce_.data() + offset * channels_ * sample_bytes_, n - offset, &md,
                                          &accepted, uint32_t(std::max<long long>(left.count(), 0)));
            if (rc != OCH_OK && rc != OCH_TIMEOUT) check(rc, "transmit");
            if (accepted) {
                first = false;
                burst_.accepted(accepted, n - offset, closes);
                offset += accepted;
                device_.tx_events().set_burst_open(burst_.starved_is_underflow());
                start_if_due();
                deadline = clock_type::now() + std::chrono::milliseconds(wait_ms);
            } else if (clock_type::now() >= deadline) {
                // Retried while a reconfiguration pauses the stream (TIMEOUT at once), like Soapy 8.5.
                throw Error("omnicore:TIMEOUT", "OmniCore transmit: " + count(empty ? 0 : done + offset) + " of " +
                                                        count(in.rows) + " samples accepted within the timeout");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        done += n;
    }
    if (clipped)
        warnings.push_back({"omnicore:TxClipped", count(clipped) + " I/Q values outside +-1 were limited to +-1"});
}

void TxSession::stop_repeat() {
    if (!repeat_.joinable()) return;
    stop_repeat_ = true;
    repeat_.join();
    device_.tx_events().set_burst_open(false);
}

void TxSession::stop(Warnings &warnings) {
    const bool was_repeating = repeating();
    stop_repeat();
    if (!stream_) return;
    if (!started_ && burst_.queued())
        warnings.push_back({"omnicore:TxNotSent", count(burst_.queued()) +
                                                          " samples were queued but never sent: less than one USB "
                                                          "transfer and no EndBurst"});
    std::string error;
    close(&error, !was_repeating);
    if (repeat_failed_) {
        repeat_failed_ = false;
        throw Error("omnicore:IO", "OmniCore repeated transmission stopped: " + repeat_error_);
    }
    if (!error.empty()) throw Error("omnicore:IO", "OmniCore stop TX: " + error);
}

void TxSession::repeat(const mxArray *x, Warnings &warnings) {
    stop(warnings);
    const Input in = inspect(x);
    if (!in.rows) throw invalid_argument("transmitRepeat: the samples are empty");
    open(in.format);
    // At least one USB transfer per och_send_burst, so short waveforms do not cost a call each.
    const size_t copies = (mtu_ + in.rows - 1) / in.rows;
    const size_t samples = in.rows * copies;
    const size_t frame = channels_ * sample_bytes_;
    repeat_samples_.resize(samples * frame);
    const size_t clipped = prepare(in, 0, in.rows, repeat_samples_.data());
    for (size_t k = 1; k < copies; ++k)
        std::memcpy(repeat_samples_.data() + k * in.rows * frame, repeat_samples_.data(), in.rows * frame);
    if (clipped)
        warnings.push_back({"omnicore:TxClipped", count(clipped) + " I/Q values outside +-1 were limited to +-1"});
    stop_repeat_ = false;
    repeat_failed_ = false;
    device_.tx_events().set_burst_open(true); // one endless burst: any starvation is an underflow
    repeat_ = std::thread(&TxSession::repeat_loop, this, samples);
}

void TxSession::repeat_loop(size_t samples) {
    // och_* only: no MATLAB API on this thread.
    const size_t frame = channels_ * sample_bytes_;
    size_t offset = 0, queued = 0;
    bool first = true;
    while (!stop_repeat_) {
        auto md = och_struct<och_tx_metadata>();
        md.flags = first ? OCH_TX_START_OF_BURST : 0;
        size_t accepted = 0;
        const int rc = och_send_burst(stream_, repeat_samples_.data() + offset * frame, samples - offset, &md,
                                      &accepted, 100);
        if (accepted) {
            first = false;
            offset = (offset + accepted) % samples;
            queued += accepted;
        }
        if (rc != OCH_OK && rc != OCH_TIMEOUT) {
            repeat_error_ = och_last_error();
            repeat_failed_ = true;
            return;
        }
        if (!started_ && queued >= mtu_) {
            if (och_start(stream_) != OCH_OK) {
                repeat_error_ = och_last_error();
                repeat_failed_ = true;
                return;
            }
            started_ = true;
        }
    }
}

mxArray *TxSession::status_struct(const TxStatus &s) const {
    const size_t n = s.ack_ticks.size();
    mxArray *times = mxCreateDoubleMatrix(n, 1, mxREAL);
    mxArray *ticks = mxCreateNumericMatrix(n, 1, mxUINT64_CLASS, mxREAL);
    for (size_t i = 0; i < n; ++i) {
        mxGetDoubles(times)[i] = tick_rate_ ? double(s.ack_ticks[i]) / tick_rate_ : 0;
        mxGetUint64s(ticks)[i] = s.ack_ticks[i];
    }
    return StructBuilder()
            .add("Underflow", make_double(double(s.underflows)))
            .add("Starved", make_double(double(s.starved)))
            .add("Late", make_double(double(s.late)))
            .add("BurstAck", make_double(double(s.acks)))
            .add("AckTime", times)
            .add("AckTicks", ticks)
            .add("Errors", make_double(double(s.errors)))
            .add("LastError", make_string(s.last_error))
            .add("Dropped", make_double(double(s.dropped)))
            .build();
}

mxArray *TxSession::status() {
    if (repeat_failed_) {
        stop_repeat();
        std::string ignored;
        close(&ignored, false);
        repeat_failed_ = false;
        throw Error("omnicore:IO", "OmniCore repeated transmission stopped: " + repeat_error_);
    }
    if (stream_) device_.tx_events().drain(pending_);
    mxArray *s = status_struct(pending_);
    pending_ = TxStatus();
    return s;
}

mxArray *TxSession::transmit(DeviceSession &device, const TxOptions &options, const mxArray *x, const TxWrite &how,
                             double timeout, Warnings &warnings) {
    TxSession session(device, options);
    const Input in = session.inspect(x);
    if (!in.rows) throw invalid_argument("transmit: the samples are empty");
    TxWrite burst = how;
    burst.end_burst = true;
    session.write(x, burst, timeout, warnings);

    // Wait for the device to report the end of the burst: its duration, the delay to a timed start,
    // and the timeout as margin.
    double seconds = timeout + 0.5 + double(in.rows) / std::max<uint32_t>(session.tick_rate_, 1);
    if (how.has_time) {
        const DeviceTime t = device.time();
        if (how.time_ticks > t.now) seconds += double(how.time_ticks - t.now) / std::max<uint32_t>(t.tick_rate, 1);
    }
    const auto deadline =
            clock_type::now() + std::chrono::milliseconds(static_cast<long long>(std::ceil(seconds * 1000)));
    TxEvents &events = device.tx_events();
    TxStatus status = session.pending_;
    const bool done = events.wait_for(TxEvents::bit(OCH_EVENT_TX_BURST_ACK) | TxEvents::bit(OCH_EVENT_TX_LATE) |
                                              TxEvents::bit(OCH_EVENT_TX_ERROR),
                                      deadline, status);
    std::string error;
    session.close(&error, true);
    status.merge(session.pending_); // events that arrived before the stream was closed
    if (!done) throw Error("omnicore:TIMEOUT", "OmniCore transmit: the device did not report the end of the burst");
    if (status.late) warnings.push_back({"omnicore:TxLate", "the burst start time had already passed"});
    if (status.underflows)
        warnings.push_back({"omnicore:TxUnderflow", count(size_t(status.underflows)) + " TX underflow(s) in the burst"});
    if (status.errors) warnings.push_back({"omnicore:TxError", status.last_error});
    return session.status_struct(status);
}
