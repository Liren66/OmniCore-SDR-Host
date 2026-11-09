// C entry points: stream defaults, create/destroy, start/stop/flush, RX, TX, timed RX, statistics.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

namespace {
    void stream(och_stream *s, unsigned direction) {
        require(s && s->args.direction == direction, OCH_INVALID, "wrong or null stream direction");
    }
}

extern "C" {
void och_default_stream_args(och_stream_args *a, och_direction d) {
    if (!a) return;
    *a = {};
    a->struct_size = sizeof(*a);
    a->direction = d;
    a->format = OCH_SC16;
    a->transfer_bytes = 196608;
    a->usb_queue_depth = 32;
    a->host_buffer_bytes = 64U * 1024 * 1024;
}

size_t och_sample_size(och_format f) {
    return f == OCH_PACKED12 ? 3 : f == OCH_SC16 ? 4 : f == OCH_FC32 ? 8 : f == OCH_SC8 ? 2 : 0;
}

int och_create_stream(och_device *d, const och_stream_args *a, och_stream **out) {
    return protect([&] {
        device(d);
        require(a && out, OCH_INVALID, "null stream arguments");
        *out = nullptr;
        require(a->struct_size == sizeof(*a) && a->direction <= OCH_TX && a->format <= OCH_SC8, OCH_INVALID,
                "stream ABI/direction/format invalid");
        require(a->transfer_bytes >= 16384 && a->transfer_bytes <= 1048576 && a->transfer_bytes % 16384 == 0,
                OCH_INVALID, "transfer_bytes must be a multiple of 16384 in 16384..1048576");
        require(a->usb_queue_depth >= 2 && a->usb_queue_depth <= 128, OCH_INVALID, "USB queue depth must be 2..128");
        require(a->host_buffer_bytes >= a->transfer_bytes * 4ULL && a->host_buffer_bytes <= 512U * 1024 * 1024,
                OCH_INVALID, "host buffer must hold at least four blocks and at most 512 MiB");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        require(d->configured, OCH_INVALID, "configure before creating streams");
        require(a->format != OCH_SC8 || iq_bits(d->config) == 8, OCH_INVALID,
                "sc8 application format requires 8-bit USB IQ mode");
        auto &slot = a->direction == OCH_RX ? d->rx : d->tx;
        require(slot == nullptr, OCH_BUSY, "only one stream per direction");
        require((a->direction == OCH_RX ? d->config.rx_mask : d->config.tx_mask) != 0, OCH_INVALID,
                "stream direction has no enabled channels");
        slot = new och_stream(d, *a);
        *out = slot;
        return OCH_OK;
    });
}

int och_destroy_stream(och_stream *s) {
    return protect([&] {
        require(s != nullptr, OCH_INVALID, "null stream");
        auto d = s->dev;
        std::unique_lock<std::mutex> lock(d->state_mutex);
        require(!s->lease, OCH_BUSY, "release RX lease before destroying stream");
        std::string issue;
        try {
            s->stop(0);
        } catch (const std::exception &e) {
            issue = e.what();
        }
        if (s->args.direction == OCH_RX)
            d->rx = nullptr;
        else
            d->tx = nullptr;
        delete s;
        if (!issue.empty()) throw Failure(OCH_IO, issue);
        return OCH_OK;
    });
}

int och_start(och_stream *s) {
    return protect([&] {
        require(s != nullptr, OCH_INVALID, "null stream");
        std::lock_guard<std::mutex> lock(s->dev->state_mutex);
        s->start();
        return OCH_OK;
    });
}

int och_stop(och_stream *s, uint32_t timeout) {
    return protect([&] {
        require(s != nullptr, OCH_INVALID, "null stream");
        std::lock_guard<std::mutex> lock(s->dev->state_mutex);
        s->stop(timeout);
        return OCH_OK;
    });
}

int och_flush(och_stream *s, uint32_t timeout) {
    return protect([&] {
        stream(s, OCH_TX);
        std::unique_lock<std::mutex> io(s->io_mutex, std::try_to_lock);
        require(io.owns_lock(), OCH_BUSY, "TX IO in progress");
        s->flush(timeout);
        return OCH_OK;
    });
}

int och_recv(och_stream *s, void *output, size_t capacity, size_t *received, och_metadata *m, uint32_t timeout) {
    return protect([&] {
        stream(s, OCH_RX);
        require(output && capacity && received, OCH_INVALID, "invalid receive buffer");
        *received = 0;
        out_struct_v(m, meta_v1);
        return s->recv(output, capacity, received, m, timeout);
    });
}

int och_rx_acquire(och_stream *s, const void **data, size_t *bytes, och_metadata *m, uint32_t timeout) {
    return protect([&] {
        stream(s, OCH_RX);
        require(data && bytes, OCH_INVALID, "null lease output");
        *data = nullptr;
        *bytes = 0;
        out_struct_v(m, meta_v1);
        return s->rx_acquire(data, bytes, m, timeout);
    });
}

int och_rx_release(och_stream *s) {
    return protect([&] {
        stream(s, OCH_RX);
        s->rx_release();
        return OCH_OK;
    });
}

namespace {
    int send_impl(och_stream *s, const void *data, size_t count, const och_tx_metadata *md, size_t *accepted,
                  uint32_t timeout) {
        return protect([&] {
            stream(s, OCH_TX);
            require(accepted && (data || !count), OCH_INVALID, "invalid TX input");
            *accepted = 0;
            uint32_t mflags = 0;
            uint64_t mtime = 0;
            if (md) {
                require(md->struct_size == sizeof(*md) &&
                                !(md->flags & ~uint32_t(OCH_TX_HAS_TIME | OCH_TX_START_OF_BURST | OCH_TX_END_OF_BURST)),
                        OCH_INVALID, "invalid och_tx_metadata");
                mflags = md->flags;
                mtime = md->time_ticks;
            }
            return s->send(data, count, mflags, mtime, accepted, timeout);
        });
    }
}

int och_send(och_stream *s, const void *data, size_t count, size_t *accepted, uint32_t timeout) {
    return send_impl(s, data, count, nullptr, accepted, timeout);
}

int och_send_burst(och_stream *s, const void *data, size_t count, const och_tx_metadata *md, size_t *accepted,
                   uint32_t timeout) {
    return send_impl(s, data, count, md, accepted, timeout);
}

int och_rx_command(och_stream *s, uint32_t flags, uint64_t time, uint32_t num_samples) {
    return protect([&] {
        stream(s, OCH_RX);
        require((s->args.start_flags & OCH_RX_TIMED) != 0, OCH_INVALID,
                "och_rx_command requires an RX stream created with OCH_RX_TIMED");
        require(!(flags & ~uint32_t(OCH_RX_NOW | OCH_RX_STOP)), OCH_INVALID, "invalid RX command flags");
        std::lock_guard<std::mutex> lock(s->dev->state_mutex);
        require(s->started, OCH_INVALID, "start the RX stream before issuing commands");
        if (flags & OCH_RX_STOP)
            s->dev->command("rx_stream_stop!");
        else
            s->dev->command("rx_stream=" + ((flags & OCH_RX_NOW) ? std::string("now") : std::to_string(time)) + " " +
                            std::to_string(num_samples));
        return OCH_OK;
    });
}

int och_get_stats(och_stream *s, och_stats *st) {
    return protect([&] {
        require(s != nullptr, OCH_INVALID, "null stream");
        out_struct(st);
        s->fill_stats(st);
        return OCH_OK;
    });
}
}
