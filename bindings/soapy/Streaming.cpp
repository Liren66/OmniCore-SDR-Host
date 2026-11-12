// Streams (SOAPY_PLAN.md section 4): RX (S2) and TX (S3).
//
// An och stream can be started only once, while SoapySDR applications activate and deactivate
// the same stream repeatedly. So setupStream only records the request, every activateStream
// creates and starts a fresh och_stream, and deactivateStream stops and destroys it.
#include "OmniCoreDevice.hpp"
#include "common/Interleave.hpp"
#include "common/Mapping.hpp"
#include "common/RxTracker.hpp"
#include "common/TxBurst.hpp"
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>

using namespace omnicore_bindings;

namespace omnicore_soapy {
    struct OmniCoreStream {
        int direction = SOAPY_SDR_RX;
        och_format format = OCH_SC16;
        size_t sample_bytes = 4; // one complex sample of one channel, in the application format
        size_t channels = 1;
        och_stream_args args{};
        size_t mtu = 0;

        // Serialises readStream with activate/deactivate. readStream holds it while blocked in
        // och_recv; deactivateStream stops the och stream first, which wakes that call.
        std::mutex io;
        // Only while active. Changed only with the device mutex and io both held, so either
        // one is enough to read it.
        och_stream *handle = nullptr;
        // Sample rate for time conversion; kept current by setSampleRate (stream_rate_changed).
        // och_metadata.tick_rate wins on RX.
        std::atomic<double> rate{0};

        // Interleaved samples from och_recv: always for two channels, and for one channel when a
        // block is held back. 8-byte elements keep sc16/fc32 alignment.
        std::vector<uint64_t> bounce;
        RxTracker tracker;

        // TX: the och stream is created at activation but started only once enough is queued
        // (SOAPY_PLAN.md 4.3). burst_open mirrors burst for readStreamStatus on another thread.
        TxBurst burst;
        bool started = false;
        std::atomic<bool> burst_open{false};
        uint64_t tx_timeouts = 0; // write timeouts since activation, for the diagnostic log

        // A block held back after an OVERFLOW or TIME_ERROR return, delivered by the next reads.
        struct Held {
            size_t count = 0, offset = 0;
            RxVerdict verdict;
            bool end_of_burst = false;
            double rate = 0;
        } held;

        void *bounce_for(size_t samples) {
            const size_t bytes = samples * channels * sample_bytes;
            if (bounce.size() * sizeof(uint64_t) < bytes) bounce.resize((bytes + 7) / 8);
            return bounce.data();
        }
    };
} // namespace omnicore_soapy

using namespace omnicore_soapy;

namespace {
    OmniCoreStream *cast(SoapySDR::Stream *s) { return reinterpret_cast<OmniCoreStream *>(s); }

    uint32_t arg_u32(const SoapySDR::Kwargs &args, const char *key, uint32_t fallback) {
        const auto it = args.find(key);
        if (it == args.end()) return fallback;
        const unsigned long long v = std::stoull(it->second);
        if (v > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument(std::string(key) + " is too large");
        return uint32_t(v);
    }

    uint32_t timeout_ms(long timeout_us) { return timeout_us <= 0 ? 0 : uint32_t((timeout_us + 999) / 1000); }

    // A TX write that accepts nothing within its timeout means the queue is not draining. Logged on
    // the 1st, 10th, 100th ... timeout of an activation, with the stream state, so a stall can be told
    // apart from a stopped stream or an application that writes faster than real time.
    void log_tx_timeout(OmniCoreStream &s, long timeout_us) {
        const uint64_t n = ++s.tx_timeouts;
        uint64_t p = 1;
        while (p < n) p *= 10;
        if (p != n || timeout_us <= 0) return;
        auto st = och_struct<och_stats>();
        if (och_get_stats(s.handle, &st) != OCH_OK) return;
        SoapySDR::logf(SOAPY_SDR_WARNING,
                       "OmniCore TX: write timed out (%llu so far, %ld us each); started=%d running=%u "
                       "queued=%zu host_queue=%llu B usb_pending=%llu underflows=%llu starved=%llu",
                       (unsigned long long)n, timeout_us, int(s.started), st.running, s.burst.queued(),
                       (unsigned long long)st.host_queue_bytes, (unsigned long long)st.usb_queue_pending,
                       (unsigned long long)st.device_underflows, (unsigned long long)st.tx_starvation_events);
    }

    // Sets the flags and time SoapySDR reports for samples [offset, offset + n) of a block.
    // gr-soapy uses timeNs without checking HAS_TIME, so it is always written.
    void report_time(const RxVerdict &v, size_t offset, bool end_of_burst, double rate, int &flags, long long &timeNs) {
        timeNs = v.time_known && rate > 0 ? SoapySDR::ticksToTimeNs((long long)(v.time_ticks + offset), rate) : 0;
        if (v.time_known) flags |= SOAPY_SDR_HAS_TIME;
        if (end_of_burst) flags |= SOAPY_SDR_END_BURST;
    }
} // namespace

std::vector<std::string> OmniCoreDevice::getStreamFormats(int, size_t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> formats{SOAPY_SDR_CS16, SOAPY_SDR_CF32};
    if (config_.usb_iq_bits == 8) formats.insert(formats.begin(), SOAPY_SDR_CS8);
    return formats;
}

// The 12-bit wire packing differs from SoapySDR's CS12 byte order, so CS16 is native: 12-bit
// samples left-aligned in 16 bits. In 8-bit wire mode CS8 is native.
std::string OmniCoreDevice::getNativeStreamFormat(int, size_t, double &fullScale) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.usb_iq_bits == 8) {
        fullScale = 128;
        return SOAPY_SDR_CS8;
    }
    fullScale = 32768;
    return SOAPY_SDR_CS16;
}

SoapySDR::ArgInfoList OmniCoreDevice::getStreamArgsInfo(int, size_t) const {
    SoapySDR::ArgInfo transfer;
    transfer.key = "transfer_bytes";
    transfer.name = "USB transfer size";
    transfer.type = SoapySDR::ArgInfo::INT;
    transfer.units = "bytes";
    transfer.value = "196608";
    transfer.description = "multiple of 16384, at most 1048576";

    SoapySDR::ArgInfo depth;
    depth.key = "usb_queue_depth";
    depth.name = "USB queue depth";
    depth.type = SoapySDR::ArgInfo::INT;
    depth.value = "32";
    depth.description = "USB transfers kept in flight, 2..128";

    SoapySDR::ArgInfo buffer;
    buffer.key = "host_buffer_bytes";
    buffer.name = "Host buffer";
    buffer.type = SoapySDR::ArgInfo::INT;
    buffer.units = "bytes";
    buffer.value = std::to_string(16u << 20);
    buffer.description = "host ring buffer, allocated at every activation; at least 4 transfers, at most 512 MiB";
    return {transfer, depth, buffer};
}

SoapySDR::Stream *OmniCoreDevice::setupStream(int direction, const std::string &format,
                                              const std::vector<size_t> &channels, const SoapySDR::Kwargs &args) {
    if (direction != SOAPY_SDR_RX && direction != SOAPY_SDR_TX) throw std::invalid_argument("invalid direction");
    const bool rx = direction == SOAPY_SDR_RX;
    const char *name = rx ? "RX" : "TX";

    auto s = std::make_unique<OmniCoreStream>();
    s->direction = direction;
    const uint32_t mask = channels_to_mask(channels, getNumChannels(direction));
    s->channels = mask == 3 ? 2 : 1;

    std::lock_guard<std::mutex> lock(mutex_);
    OmniCoreStream *&slot = rx ? rx_stream_ : tx_stream_;
    OmniCoreStream *other = rx ? tx_stream_ : rx_stream_;
    if (slot) throw std::runtime_error(std::string("OmniCore: a ") + name + " stream is already set up; close it first");

    const bool iq8 = config_.usb_iq_bits == 8;
    if (format == SOAPY_SDR_CS16)
        s->format = OCH_SC16;
    else if (format == SOAPY_SDR_CF32)
        s->format = OCH_FC32;
    else if (format == SOAPY_SDR_CS8 && iq8)
        s->format = OCH_SC8;
    else
        throw std::invalid_argument("OmniCore: unsupported stream format " + format +
                                    (format == SOAPY_SDR_CS8 ? " (CS8 needs setting iq_bits=8)" : ""));
    s->sample_bytes = och_sample_size(s->format);
    if (iq8 && mask != 3) throw std::invalid_argument("OmniCore: 8-bit IQ (iq_bits=8) streams both channels");

    // The core accepts a channel-mask change only while no och_stream exists (SOAPY_PLAN.md 3.4).
    if (mask != (rx ? config_.rx_mask : config_.tx_mask)) {
        if (other && other->handle)
            throw std::runtime_error(std::string("OmniCore: ") + name + " needs " + (rx ? "rx_mask=" : "tx_mask=") +
                                     std::to_string(mask) + " but " + (rx ? "TX" : "RX") +
                                     " is streaming; set rx_mask and tx_mask in the device arguments "
                                     "so both directions are configured before either starts");
        apply_locked([&](och_config &c) { (rx ? c.rx_mask : c.tx_mask) = mask; });
    }

    och_default_stream_args(&s->args, rx ? OCH_RX : OCH_TX);
    s->args.format = s->format;
    s->args.host_buffer_bytes = 16u << 20;
    s->args.transfer_bytes = arg_u32(args, "transfer_bytes", s->args.transfer_bytes);
    s->args.usb_queue_depth = arg_u32(args, "usb_queue_depth", s->args.usb_queue_depth);
    s->args.host_buffer_bytes = arg_u32(args, "host_buffer_bytes", s->args.host_buffer_bytes);

    // Samples per channel in one USB transfer: each 16 KiB block carries 16368 payload bytes.
    const size_t wire_bytes = iq8 ? 2 : 3;
    s->mtu = std::max<size_t>(1, s->args.transfer_bytes / 16384 * 16368 / (wire_bytes * s->channels));
    s->bounce_for(s->mtu);

    slot = s.release();
    return reinterpret_cast<SoapySDR::Stream *>(slot);
}

void OmniCoreDevice::closeStream(SoapySDR::Stream *stream) {
    auto *s = cast(stream);
    deactivateStream(stream, 0, 0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (rx_stream_ == s) rx_stream_ = nullptr;
        if (tx_stream_ == s) tx_stream_ = nullptr;
    }
    delete s;
}

void OmniCoreDevice::stream_rate_changed() {
    for (OmniCoreStream *s: {rx_stream_, tx_stream_})
        if (s) s->rate = config_.sample_rate;
}

size_t OmniCoreDevice::getStreamMTU(SoapySDR::Stream *stream) const { return cast(stream)->mtu; }

int OmniCoreDevice::activateStream(SoapySDR::Stream *stream, int flags, long long timeNs, size_t numElems) {
    auto *s = cast(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    std::lock_guard<std::mutex> io(s->io);
    if (s->handle) return 0;

    if (s->direction == SOAPY_SDR_TX) {
        // Created now so writes can queue; started by writeStream (och_start needs queued samples).
        check(och_create_stream(dev_, &s->args, &s->handle), "create TX stream");
        s->rate = config_.sample_rate;
        s->started = false;
        s->burst.reset();
        s->tx_timeouts = 0;
        s->burst_open = false;
        clear_tx_events();
        tx_listening_ = true;
        return 0;
    }

    och_stream_args a = s->args;
    // HAS_TIME: start at timeNs. numElems > 0: finite burst. Either needs a timed RX stream.
    if (flags & SOAPY_SDR_HAS_TIME) {
        if (timeNs < 0) throw std::invalid_argument("OmniCore: negative start time");
        const auto ticks = uint64_t(SoapySDR::timeNsToTicks(timeNs, tick_rate()));
        a.start_flags = OCH_RX_TIMED;
        a.start_time_lo = uint32_t(ticks);
        a.start_time_hi = uint32_t(ticks >> 32);
    } else if (numElems) {
        a.start_flags = OCH_RX_TIMED | OCH_RX_NOW;
    }
    if (numElems > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("OmniCore: burst too long for one RX command");
    a.num_samples = uint32_t(numElems);

    // Errors throw rather than return a code: gr-soapy ignores activateStream's return value.
    check(och_create_stream(dev_, &a, &s->handle), "create RX stream");
    if (och_start(s->handle) != OCH_OK) {
        const std::string error = och_last_error();
        och_destroy_stream(s->handle);
        s->handle = nullptr;
        throw std::runtime_error("OmniCore start RX: " + error);
    }
    s->rate = config_.sample_rate;
    s->tracker.reset();
    s->held = {};
    return 0;
}

int OmniCoreDevice::deactivateStream(SoapySDR::Stream *stream, int, long long) {
    auto *s = cast(stream);
    std::lock_guard<std::mutex> lock(mutex_);
    och_stream *const handle = s->handle;
    if (!handle) return 0;
    std::string error;
    if (s->direction == SOAPY_SDR_TX) {
        tx_listening_ = false;
        if (s->started) {
            // Send what is queued and close the last block (with end-of-burst) before stopping.
            if (och_flush(handle, 1000) != OCH_OK)
                SoapySDR::logf(SOAPY_SDR_DEBUG, "OmniCore: TX flush before stop: %s", och_last_error());
        } else if (s->burst.queued()) {
            SoapySDR::logf(SOAPY_SDR_WARNING,
                           "OmniCore: %zu TX samples were queued but never sent: less than one USB transfer "
                           "and no END_BURST",
                           s->burst.queued());
        }
    }
    // Stop first, without the io lock, so a readStream blocked in och_recv returns.
    if (och_stop(handle, 3000) != OCH_OK) error = och_last_error();
    {
        std::lock_guard<std::mutex> io(s->io);
        if (och_destroy_stream(handle) != OCH_OK && error.empty()) error = och_last_error();
        s->handle = nullptr;
        s->held = {};
    }
    if (!error.empty()) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore: stopping the stream: %s", error.c_str());
        return SOAPY_SDR_STREAM_ERROR;
    }
    return 0;
}

int OmniCoreDevice::readStream(SoapySDR::Stream *stream, void *const *buffs, size_t numElems, int &flags,
                               long long &timeNs, long timeoutUs) {
    auto *s = cast(stream);
    flags = 0;
    if (s->direction != SOAPY_SDR_RX) return SOAPY_SDR_NOT_SUPPORTED;
    std::lock_guard<std::mutex> io(s->io);
    if (!s->handle) return SOAPY_SDR_STREAM_ERROR;
    if (numElems == 0) return 0;

    auto &held = s->held;
    if (held.count) {
        const size_t n = std::min(numElems, held.count - held.offset);
        deinterleave(s->bounce.data(), buffs, s->channels, s->sample_bytes, held.offset, n);
        report_time(held.verdict, held.offset, held.end_of_burst && held.offset + n == held.count, held.rate, flags,
                    timeNs);
        held.offset += n;
        if (held.offset == held.count) held = {};
        return int(n);
    }

    // One channel reads straight into the caller's buffer; two go through the bounce buffer.
    const bool direct = s->channels == 1;
    const size_t capacity = std::min(direct ? numElems : s->mtu, std::min(numElems, size_t(INT_MAX)));
    void *target = direct ? buffs[0] : s->bounce.data();
    auto meta = och_struct<och_metadata>();
    size_t n = 0;
    const int rc = och_recv(s->handle, target, capacity, &n, &meta, timeout_ms(timeoutUs));
    if (rc == OCH_TIMEOUT) return SOAPY_SDR_TIMEOUT;
    if (rc == OCH_STOPPED) return SOAPY_SDR_STREAM_ERROR; // deactivateStream is stopping it
    if (rc != OCH_OK) {
        SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore RX: %s", och_last_error());
        return SOAPY_SDR_STREAM_ERROR;
    }

    const RxVerdict v = s->tracker.observe(meta, n);
    const double rate = meta.tick_rate ? double(meta.tick_rate) : s->rate.load();
    const bool eob = (meta.flags & OCH_META_END_OF_BURST) != 0;
    if (v.overflow || v.late) {
        // Report the error alone now and the samples, with their time, on the next reads; that is
        // when gr-soapy re-tags rx_time.
        if (direct) std::memcpy(s->bounce_for(n), buffs[0], n * s->sample_bytes);
        held.count = n;
        held.offset = 0;
        held.verdict = v;
        held.end_of_burst = eob;
        held.rate = rate;
        return v.late ? SOAPY_SDR_TIME_ERROR : SOAPY_SDR_OVERFLOW;
    }
    if (!direct) deinterleave(s->bounce.data(), buffs, s->channels, s->sample_bytes, 0, n);
    report_time(v, 0, eob, rate, flags, timeNs);
    return int(n);
}

int OmniCoreDevice::writeStream(SoapySDR::Stream *stream, const void *const *buffs, size_t numElems, int &flags,
                                long long timeNs, long timeoutUs) {
    auto *s = cast(stream);
    if (s->direction != SOAPY_SDR_TX) return SOAPY_SDR_NOT_SUPPORTED;
    std::lock_guard<std::mutex> io(s->io);
    if (!s->handle) return SOAPY_SDR_STREAM_ERROR;

    const bool has_time = (flags & SOAPY_SDR_HAS_TIME) != 0;
    const bool end_burst = (flags & SOAPY_SDR_END_BURST) != 0;
    if (numElems == 0 && !end_burst) return 0;

    // A zero-length END_BURST cannot mark a block in the core, so it ends the burst with one
    // zero sample per channel.
    const bool empty_end = numElems == 0;
    size_t n = empty_end ? 1 : std::min(numElems, size_t(INT_MAX));
    const void *src;
    if (empty_end) {
        src = std::memset(s->bounce_for(1), 0, s->channels * s->sample_bytes);
    } else if (s->channels == 1) {
        src = buffs[0];
    } else {
        n = std::min(n, s->mtu);
        src = s->bounce.data();
        interleave(buffs, s->bounce.data(), s->channels, s->sample_bytes, n);
    }
    // A partly accepted write keeps END_BURST off, so the burst closes on the call that sends the rest.
    const bool closes = end_burst && n == (empty_end ? 1 : numElems);

    auto md = och_struct<och_tx_metadata>();
    md.flags = s->burst.flags(has_time, closes);
    if (has_time) {
        if (timeNs < 0) return SOAPY_SDR_TIME_ERROR;
        md.time_ticks = uint64_t(SoapySDR::timeNsToTicks(timeNs, s->rate.load()));
    }
    // While a TX reconfigure runs (LO, bandwidth) the core pauses the stream and och_send_burst returns
    // OCH_TIMEOUT at once instead of waiting. Keep retrying until the caller's timeout has really
    // passed: SoapySDR callers (gr-soapy) treat TIMEOUT as "waited the full time" and retry at once,
    // which otherwise spins and floods the log (SOAPY_PLAN.md 8.5).
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::microseconds(std::max(timeoutUs, 0L));
    size_t accepted = 0;
    int rc;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - clock::now()).count();
        rc = och_send_burst(s->handle, src, n, &md, &accepted, timeout_ms(std::max<long>(long(left), 0)));
        if (rc != OCH_TIMEOUT || accepted != 0 || clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (rc == OCH_TIMEOUT && accepted == 0) {
        log_tx_timeout(*s, timeoutUs);
        return SOAPY_SDR_TIMEOUT;
    }
    if (rc != OCH_OK && rc != OCH_TIMEOUT) {
        if (rc != OCH_STOPPED) SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore TX: %s", och_last_error());
        return SOAPY_SDR_STREAM_ERROR;
    }
    const bool closed = s->burst.accepted(accepted, n, closes);
    s->burst_open = s->burst.starved_is_underflow();
    if (end_burst && !closed) flags &= ~SOAPY_SDR_END_BURST;

    if (!s->started && s->burst.should_start(s->mtu)) {
        // Blocks until the FPGA holds about one block (core TX prefill), at most ~5 s.
        if (och_start(s->handle) != OCH_OK) {
            SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore start TX: %s", och_last_error());
            return SOAPY_SDR_STREAM_ERROR;
        }
        s->started = true;
    }
    return empty_end ? 0 : int(accepted);
}

int OmniCoreDevice::readStreamStatus(SoapySDR::Stream *stream, size_t &chanMask, int &flags, long long &timeNs,
                                     long timeoutUs) {
    auto *s = cast(stream);
    if (s->direction != SOAPY_SDR_TX) return SOAPY_SDR_NOT_SUPPORTED;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(std::max(0L, timeoutUs));
    std::unique_lock<std::mutex> lock(tx_events_mutex_);
    for (;;) {
        if (!tx_events_cv_.wait_until(lock, deadline, [&] { return !tx_events_.empty(); }))
            return SOAPY_SDR_TIMEOUT;
        const och_event e = tx_events_.front();
        tx_events_.pop_front();

        chanMask = (size_t(1) << s->channels) - 1;
        flags = 0;
        timeNs = 0;
        const double rate = s->rate.load();
        if (e.hw_time && rate > 0) {
            timeNs = SoapySDR::ticksToTimeNs((long long)e.hw_time, rate);
            flags |= SOAPY_SDR_HAS_TIME;
        }
        switch (e.type) {
        case OCH_EVENT_TX_BURST_ACK:
            flags |= SOAPY_SDR_END_BURST;
            return 0;
        case OCH_EVENT_TX_UNDERFLOW:
            return SOAPY_SDR_UNDERFLOW;
        case OCH_EVENT_TX_STARVED:
            if (s->burst_open) return SOAPY_SDR_UNDERFLOW;
            continue; // idle between bursts
        case OCH_EVENT_TX_LATE:
            return SOAPY_SDR_TIME_ERROR;
        default:
            return SOAPY_SDR_STREAM_ERROR;
        }
    }
}
