#include "RxSession.hpp"
#include "MxConvert.hpp"
#include "common/Interleave.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

using namespace omnicore_mex;

namespace {
    // Samples per channel per och_recv through the bounce buffer: 64 Ki samples, at most 1 MiB for
    // two fc32 channels. Blocks are 16 KiB on the wire, so this never splits one.
    constexpr size_t bounce_samples = 65536;

    och_format wire_format(SampleType t) {
        switch (t) {
        case SampleType::Int16:
            return OCH_SC16;
        case SampleType::Int8:
            return OCH_SC8;
        default:
            return OCH_FC32;
        }
    }

    size_t channel_count(uint32_t mask) { return size_t((mask & 1) != 0) + size_t((mask & 2) != 0); }
} // namespace

namespace omnicore_mex {
    SampleType sample_type(const std::string &name) {
        if (name == "single") return SampleType::Single;
        if (name == "double") return SampleType::Double;
        if (name == "int16") return SampleType::Int16;
        if (name == "int8") return SampleType::Int8;
        throw invalid_argument("OutputDataType must be 'single', 'double', 'int16' or 'int8'");
    }

    const char *sample_type_name(SampleType t) {
        switch (t) {
        case SampleType::Double:
            return "double";
        case SampleType::Int16:
            return "int16";
        case SampleType::Int8:
            return "int8";
        default:
            return "single";
        }
    }

    // The N x channels complex output array. Owns it until release(), so an error on the way frees it.
    class RxOutput {
    public:
        RxOutput(SampleType type, size_t rows, size_t channels) : type_(type), rows_(rows), channels_(channels) {
            mxClassID cls = mxSINGLE_CLASS;
            switch (type) {
            case SampleType::Single:
                cls = mxSINGLE_CLASS, element_ = sizeof(mxComplexSingle);
                break;
            case SampleType::Double:
                cls = mxDOUBLE_CLASS, element_ = sizeof(mxComplexDouble);
                break;
            case SampleType::Int16:
                cls = mxINT16_CLASS, element_ = sizeof(mxComplexInt16);
                break;
            case SampleType::Int8:
                cls = mxINT8_CLASS, element_ = sizeof(mxComplexInt8);
                break;
            }
            // Uninitialised: every row returned is written, the rest is cut off in release().
            array_ = mxCreateUninitNumericMatrix(rows, channels, cls, mxCOMPLEX);
            switch (type) {
            case SampleType::Single:
                data_ = reinterpret_cast<uint8_t *>(mxGetComplexSingles(array_));
                break;
            case SampleType::Double:
                data_ = reinterpret_cast<uint8_t *>(mxGetComplexDoubles(array_));
                break;
            case SampleType::Int16:
                data_ = reinterpret_cast<uint8_t *>(mxGetComplexInt16s(array_));
                break;
            case SampleType::Int8:
                data_ = reinterpret_cast<uint8_t *>(mxGetComplexInt8s(array_));
                break;
            }
        }
        ~RxOutput() {
            if (array_) mxDestroyArray(array_);
        }
        RxOutput(const RxOutput &) = delete;
        RxOutput &operator=(const RxOutput &) = delete;

        size_t rows() const { return rows_; }
        // och_recv can write one channel of the requested format straight into the column.
        bool direct() const { return channels_ == 1 && type_ != SampleType::Double; }
        void *at(size_t row) { return data_ + row * element_; }

        // n interleaved och samples (fc32 for double) -> rows [row, row + n) of every column.
        void store(const void *src, size_t row, size_t n) {
            if (type_ == SampleType::Double) {
                const float *in = static_cast<const float *>(src);
                for (size_t c = 0; c < channels_; ++c) {
                    auto *out = reinterpret_cast<mxComplexDouble *>(data_) + c * rows_ + row;
                    for (size_t k = 0; k < n; ++k) {
                        out[k].real = in[(k * channels_ + c) * 2];
                        out[k].imag = in[(k * channels_ + c) * 2 + 1];
                    }
                }
                return;
            }
            void *columns[2] = {};
            for (size_t c = 0; c < channels_; ++c) columns[c] = data_ + (c * rows_ + row) * element_;
            omnicore_bindings::deinterleave(src, columns, channels_, element_, 0, n);
        }

        // Hands the array to MATLAB, cut to the rows actually received.
        mxArray *release(size_t rows) {
            if (rows < rows_) {
                for (size_t c = 1; c < channels_; ++c)
                    std::memmove(data_ + c * rows * element_, data_ + c * rows_ * element_, rows * element_);
                mxSetM(array_, rows);
            }
            mxArray *a = array_;
            array_ = nullptr;
            return a;
        }

    private:
        SampleType type_;
        size_t rows_, channels_, element_ = 0;
        mxArray *array_ = nullptr;
        uint8_t *data_ = nullptr;
    };
} // namespace omnicore_mex

RxSession::RxSession(DeviceSession &device, const RxOptions &options) : device_(device), options_(options) {}

RxSession::~RxSession() { close(nullptr); }

void RxSession::open(uint32_t start_flags, uint32_t num_samples, uint64_t start_ticks) {
    if (stream_) throw Error("omnicore:BUSY", "the RX stream is already running");
    const och_config c = device_.config();
    channels_ = channel_count(c.rx_mask);
    if (!channels_) throw invalid_argument("RxChannels is empty: enable an RX channel first");
    if (options_.type == SampleType::Int8 && c.usb_iq_bits != 8)
        throw invalid_argument("OutputDataType 'int8' needs IQBits 8");

    och_stream_args a = och_struct<och_stream_args>();
    och_default_stream_args(&a, OCH_RX);
    a.format = wire_format(options_.type);
    if (options_.buffer_bytes) a.host_buffer_bytes = options_.buffer_bytes;
    a.start_flags = start_flags;
    a.num_samples = num_samples;
    a.start_time_lo = uint32_t(start_ticks);
    a.start_time_hi = uint32_t(start_ticks >> 32);
    check(och_create_stream(device_.handle(), &a, &stream_), "create RX stream");
    if (och_start(stream_) != OCH_OK) {
        const std::string error = och_last_error();
        och_destroy_stream(stream_);
        stream_ = nullptr;
        throw Error("omnicore:IO", "OmniCore start RX: " + error);
    }
    sample_rate_ = c.sample_rate;
    tracker_.reset();
    const size_t bytes = bounce_samples * channels_ * och_sample_size(och_format(a.format));
    if (bounce_.size() < bytes) bounce_.resize(bytes);
}

void RxSession::close(std::string *error) {
    if (!stream_) return;
    const int stopped = och_stop(stream_, 3000);
    if (stopped != OCH_OK && error) *error = och_last_error();
    const int destroyed = och_destroy_stream(stream_);
    if (destroyed != OCH_OK && error && error->empty()) *error = och_last_error();
    stream_ = nullptr;
}

void RxSession::start(bool has_time, uint64_t ticks) { open(has_time ? OCH_RX_TIMED : 0, 0, ticks); }

void RxSession::stop() {
    std::string error;
    close(&error);
    if (!error.empty()) throw Error("omnicore:IO", "OmniCore stop RX: " + error);
}

size_t RxSession::pump(RxOutput &out, uint32_t first_wait_ms, uint32_t wait_ms, RxReport &report, bool &timed_out) {
    size_t rows = 0;
    timed_out = false;
    while (rows < out.rows()) {
        size_t capacity = out.rows() - rows;
        void *target = nullptr;
        if (out.direct()) {
            target = out.at(rows);
        } else {
            capacity = std::min(capacity, bounce_samples);
            target = bounce_.data();
        }
        auto meta = och_struct<och_metadata>();
        size_t n = 0;
        const int rc = och_recv(stream_, target, capacity, &n, &meta, report.blocks ? wait_ms : first_wait_ms);
        if (rc == OCH_TIMEOUT) {
            timed_out = true;
            break;
        }
        if (rc == OCH_STOPPED) break; // a finite command delivered everything
        check(rc, "receive");
        if (!n) continue;
        if (!out.direct()) out.store(target, rows, n);
        tracker_.observe(meta, n, rows, report);
        rows += n;
        if (report.end_of_burst) break;
    }
    return rows;
}

mxArray *RxSession::meta_struct(const RxReport &r) const {
    const double rate = r.tick_rate ? double(r.tick_rate) : double(sample_rate_);
    mxArray *ticks = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
    *mxGetUint64s(ticks) = r.time_ticks;
    mxArray *gaps = mxCreateDoubleMatrix(r.gaps.size(), 2, mxREAL);
    double *g = mxGetDoubles(gaps);
    for (size_t i = 0; i < r.gaps.size(); ++i) {
        g[i] = r.gaps[i].index;
        g[i + r.gaps.size()] = r.gaps[i].lost;
    }
    return StructBuilder()
            .add("Time", make_double(r.time_known && rate > 0 ? double(r.time_ticks) / rate
                                                               : std::numeric_limits<double>::quiet_NaN()))
            .add("TimeTicks", ticks)
            .add("Overflow", make_bool(r.overflow()))
            .add("Gaps", gaps)
            .add("Late", make_bool(r.late))
            .add("Reconfigured", make_bool(r.reconfigured))
            .build();
}

void RxSession::read(size_t n, double timeout, mxArray **samples, mxArray **meta) {
    const uint32_t wait_ms = timeout_ms(timeout);
    if (!stream_) throw Error("omnicore:STOPPED", "the RX stream is not running: call start first");
    RxOutput out(options_.type, n, channels_);
    RxReport report;
    bool timed_out = false;
    const size_t rows = n ? pump(out, wait_ms, wait_ms, report, timed_out) : 0;
    if (n && !rows && timed_out)
        throw Error("omnicore:TIMEOUT", "OmniCore receive: no samples within the timeout");
    *samples = out.release(rows);
    if (meta) *meta = meta_struct(report);
}

void RxSession::receive(DeviceSession &device, const RxOptions &options, size_t n, double timeout, bool has_time,
                        uint64_t ticks, mxArray **samples, mxArray **meta) {
    const uint32_t wait_ms = timeout_ms(timeout);
    if (n == 0 || n > std::numeric_limits<uint32_t>::max())
        throw invalid_argument("receive: N must be 1 to 4294967295 samples");
    RxSession session(device, options);
    // The output first: allocating gigabytes can fail, and should before the device streams.
    const size_t channels = channel_count(device.config().rx_mask);
    RxOutput out(options.type, n, channels ? channels : 1);
    // The first block also waits for the stream to start (USB queue setup, a few ms) and for a
    // timed start to come.
    uint64_t first_wait = uint64_t(wait_ms) + 1000;
    if (has_time) {
        const DeviceTime t = device.time();
        if (ticks > t.now && t.tick_rate)
            first_wait += uint64_t(std::ceil(double(ticks - t.now) * 1000 / t.tick_rate));
        if (first_wait > 3600000) throw invalid_argument("StartTime is more than an hour ahead of the device time");
    }
    session.open(has_time ? OCH_RX_TIMED : OCH_RX_TIMED | OCH_RX_NOW, uint32_t(n), ticks);
    RxReport report;
    bool timed_out = false;
    const size_t rows = session.pump(out, uint32_t(first_wait), wait_ms, report, timed_out);
    if (rows < n && !report.end_of_burst && timed_out)
        throw Error("omnicore:TIMEOUT", "OmniCore receive: " + std::to_string(rows) + " of " + std::to_string(n) +
                                                " samples within the timeout");
    std::string error;
    session.close(&error);
    *samples = out.release(rows);
    if (meta) *meta = session.meta_struct(report);
}
