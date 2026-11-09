// Application-thread side of a stream: och_recv / och_rx_acquire / och_send and statistics.
// Each call takes io_mutex with try_lock and returns OCH_TIMEOUT while a reconfiguration holds it
// (see reconfigure.cpp). The USB worker side is in stream.cpp.
#include "internal.hpp"
#include <limits>
using namespace och::core;

namespace {
    // Copies only the caller's struct_size bytes, so shorter pre-0.6.1 och_metadata layouts keep working.
    void put_meta(och_metadata *out, const och_metadata &m) {
        const auto n = out->struct_size;
        memcpy(out, &m, n);
        out->struct_size = n;
    }
} // namespace

och_metadata och_stream::adjusted(const Ring::Block &b) const {
    auto m = b.meta;
    m.sample_index += recv_offset / group;
    if (recv_offset) {
        const bool timed = (m.flags & OCH_META_HAS_TIME) != 0;
        m.flags = timed ? uint32_t(OCH_META_HAS_TIME | (m.flags & OCH_META_END_OF_BURST))
                        : uint32_t(OCH_META_TIME_UNKNOWN);
        m.host_dropped_samples = 0;
        m.device_dropped_samples = 0;
        if (timed) m.time_ticks += recv_offset / group;
    }
    return m;
}

int och_stream::await_rx(uint32_t timeout) {
    if (reconfiguring) return OCH_TIMEOUT;
    if (ring.read()) return OCH_OK;
    if (!running) {
        auto e = error();
        if (!e.empty()) throw Failure(OCH_IO, e);
        return OCH_STOPPED;
    }
    const bool ready = ring.wait_read(timeout, stop_requested, &reconfiguring);
    if (reconfiguring) return OCH_TIMEOUT;
    if (ready) return OCH_OK;
    if (!running) {
        auto e = error();
        if (!e.empty()) throw Failure(OCH_IO, e);
        return OCH_STOPPED;
    }
    return OCH_TIMEOUT;
}

int och_stream::recv(void *output, size_t capacity, size_t *received, och_metadata *m, uint32_t timeout) {
    std::unique_lock<std::mutex> io(io_mutex, std::try_to_lock);
    if (!io.owns_lock() || reconfiguring) return OCH_TIMEOUT;
    require(!lease, OCH_BUSY, "release zero-copy lease before recv");
    int rc = await_rx(timeout);
    if (rc) return static_cast<och_result>(rc);
    auto b = ring.read();
    size_t frames = std::min(capacity, (b->size - recv_offset) / group);
    auto p = b->data.get() + recv_offset;
    put_meta(m, adjusted(*b));
    if (args.format == OCH_SC8)
        memcpy(output, p, frames * group);
    else if (args.format == OCH_PACKED12) {
        if (wire_bytes == 2)
            och::dsp::unpack8_packed12(p, static_cast<uint8_t *>(output), frames * nchan);
        else
            memcpy(output, p, frames * group);
    } else if (args.format == OCH_SC16) {
        require(reinterpret_cast<uintptr_t>(output) % alignof(int16_t) == 0, OCH_INVALID, "sc16 buffer alignment");
        (wire_bytes == 2 ? och::dsp::unpack8_sc16 : och::dsp::unpack_sc16)(p, static_cast<int16_t *>(output),
                                                                           frames * nchan);
    } else {
        require(reinterpret_cast<uintptr_t>(output) % alignof(float) == 0, OCH_INVALID, "fc32 buffer alignment");
        (wire_bytes == 2 ? och::dsp::unpack8_fc32 : och::dsp::unpack_fc32)(p, static_cast<float *>(output),
                                                                           frames * nchan);
    }
    recv_offset += frames * group;
    samples += frames;
    *received = frames;
    if (recv_offset == b->size) {
        ring.release();
        recv_offset = 0;
    }
    return OCH_OK;
}

int och_stream::rx_acquire(const void **data, size_t *bytes, och_metadata *m, uint32_t timeout) {
    std::unique_lock<std::mutex> io(io_mutex, std::try_to_lock);
    if (!io.owns_lock() || reconfiguring) return OCH_TIMEOUT;
    require(!lease, OCH_BUSY, "only one RX lease allowed");
    int rc = await_rx(timeout);
    if (rc) return static_cast<och_result>(rc);
    auto b = ring.read();
    *data = b->data.get() + recv_offset;
    *bytes = b->size - recv_offset;
    put_meta(m, adjusted(*b));
    lease = true;
    return OCH_OK;
}

void och_stream::rx_release() {
    std::lock_guard<std::mutex> io(io_mutex);
    require(lease, OCH_INVALID, "no RX lease");
    auto b = ring.read();
    samples += (b->size - recv_offset) / group;
    ring.release();
    recv_offset = 0;
    lease = false;
}

int och_stream::send(const void *data, size_t count, uint32_t mflags, uint64_t mtime, size_t *accepted,
                     uint32_t timeout) {
    std::unique_lock<std::mutex> io(io_mutex, std::try_to_lock);
    if (!io.owns_lock() || reconfiguring) return OCH_TIMEOUT;
    require(count <= std::numeric_limits<size_t>::max() / (8 * nchan), OCH_INVALID, "sample count overflow");
    require(!flushing, OCH_BUSY, "TX flush in progress");
    if (stop_requested) return OCH_STOPPED;
    size_t width = och_sample_size(static_cast<och_format>(args.format));
    auto p = static_cast<const uint8_t *>(data);
    if (args.format == OCH_SC16 || args.format == OCH_FC32)
        require(reinterpret_cast<uintptr_t>(data) % (args.format == OCH_SC16 ? alignof(int16_t) : alignof(float)) == 0,
                OCH_INVALID, "TX sample buffer alignment");
    auto until = now_ns() + uint64_t(timeout) * 1000000;
    while (*accepted < count) {
        if (reconfiguring) return *accepted ? OCH_OK : OCH_TIMEOUT;
        if (stop_requested) return *accepted ? OCH_OK : OCH_STOPPED;
        auto b = ring.write();
        if (!b) {
            if (ever_started && !running) throw Failure(OCH_IO, "TX worker stopped: " + error());
            auto now = now_ns();
            if (now >= until) return *accepted ? OCH_OK : OCH_TIMEOUT;
            auto remaining = static_cast<uint32_t>(std::max<uint64_t>(1, (until - now) / 1000000));
            ring.wait_write(remaining, stop_requested, &reconfiguring);
            if (reconfiguring) return *accepted ? OCH_OK : OCH_TIMEOUT;
            if (stop_requested) return *accepted ? OCH_OK : OCH_STOPPED;
            continue;
        }
        size_t frames = std::min(count - *accepted, ring.block_bytes() / group);
        auto src = p + *accepted * width * nchan;
        if (args.format == OCH_SC8)
            memcpy(b->data.get(), src, frames * group);
        else if (args.format == OCH_PACKED12) {
            if (wire_bytes == 2)
                och::dsp::pack8_packed12(src, b->data.get(), frames * nchan);
            else
                memcpy(b->data.get(), src, frames * group);
        } else if (args.format == OCH_SC16)
            (wire_bytes == 2 ? och::dsp::pack8_sc16 : och::dsp::pack_sc16)(reinterpret_cast<const int16_t *>(src),
                                                                           b->data.get(), frames * nchan);
        else
            (wire_bytes == 2 ? och::dsp::pack8_fc32 : och::dsp::pack_fc32)(reinterpret_cast<const float *>(src),
                                                                           b->data.get(), frames * nchan);
        och_metadata tag{};
        if (*accepted == 0) {
            tag.flags = mflags & (OCH_TX_HAS_TIME | OCH_TX_START_OF_BURST);
            tag.time_ticks = mtime;
        }
        if (*accepted + frames == count) tag.flags |= mflags & OCH_TX_END_OF_BURST;
        ring.publish(frames * group, tag);
        *accepted += frames;
        samples += frames;
        auto used = ring.queued();
        if (used > peak) peak = used;
    }
    return OCH_OK;
}

void och_stream::fill_stats(och_stats *st) {
    *st = {};
    st->struct_size = sizeof(*st);
    st->running = running;
    st->usb_bytes = usb_bytes;
    st->samples = samples;
    st->host_dropped_samples = host_dropped;
    st->headers = headers;
    st->resyncs = resyncs;
    st->host_queue_bytes = ring.queued();
    st->host_queue_peak_bytes = peak;
    st->usb_queue_pending = pending;
    st->transfer_errors = errors;
    st->device_overflows = dev->overflows;
    st->device_underflows = dev->underflows;
    st->event_drops = dev->event_drops;
    st->tx_starvation_events = starved;
    auto began = started_ns.load();
    auto end = running ? now_ns() : ended_ns.load();
    st->elapsed_ns = began && end >= began ? end - began : 0;
    st->tx_padding_samples = padding;
}
