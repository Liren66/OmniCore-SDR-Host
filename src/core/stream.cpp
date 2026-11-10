#include "internal.hpp"
#include <cstdio>
using namespace och::core;
using namespace och::transport;

Stream::Stream(Device *d, const och_stream_args &a)
    : dev(d), args(a), nchan(channels(a.direction == OCH_RX ? d->config.rx_mask : d->config.tx_mask)),
      wire_bytes(iq_bits(d->config) == 8 ? 2 : 3), group(nchan * wire_bytes),
      ring(std::max<size_t>(2, a.host_buffer_bytes / a.transfer_bytes), a.transfer_bytes),
      stop_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), rx_state(nchan * (iq_bits(d->config) == 8 ? 2 : 3)) {
    require(stop_event.h != nullptr, OCH_IO, "stream stop event allocation failed");
    const bool block_xfers = a.direction == OCH_RX && (a.start_flags & OCH_RX_TIMED);
    xfer_bytes = block_xfers ? och::dsp::frame_block_bytes : a.transfer_bytes;
    const unsigned nslots = block_xfers ? 128u : args.usb_queue_depth;
    slots.reserve(nslots);
    for (unsigned i = 0; i < nslots; ++i) slots.push_back(std::make_unique<Slot>(dev->usb, xfer_bytes));
}

Stream::~Stream() {
    stop_requested = true;
    SetEvent(stop_event.h);
    ring.wake();
    if (worker.joinable()) worker.join();
}

void Stream::fail(const std::string &s) {
    {
        std::lock_guard<std::mutex> lock(failure_mutex);
        failure = s;
    }
    ++stats.errors;
    dev->emit(OCH_EVENT_TRANSPORT, s);
}

std::string Stream::error() {
    std::lock_guard<std::mutex> lock(failure_mutex);
    return failure;
}

void Stream::frame_sink(void *self, const och::dsp::FrameHeader &h, const uint8_t *p, size_t n) {
    static_cast<Stream *>(self)->rx_frame(h, p, n);
}

// One framed block: whole samples starting at h.time. A new segment (and epoch)
// starts at SOB, GAP, LATE or any time discontinuity; GAP gives the exact
// number of samples the FPGA dropped (difference of timestamps).
void Stream::rx_frame(const och::dsp::FrameHeader &h, const uint8_t *p, size_t n) {
    using namespace och::dsp;
    const bool continuous = rx_state.have_time && h.time == rx_state.next_time &&
                            !(h.flags & (frame_rx_sob | frame_rx_gap | frame_rx_late));
    if (!continuous) {
        publish_rx();
        ++rx_state.fr_epochs;
        rx_state.epoch = rx_state.epoch_base + rx_state.fr_epochs;
        rx_state.index = 0;
        rx_state.flags_pending = (rx_state.flags_pending & OCH_META_HOST_OVERFLOW) | OCH_META_START;
        rx_state.gap_samples = 0;
        if (h.flags & frame_rx_gap) {
            rx_state.flags_pending |= OCH_META_DISCONTINUITY | OCH_META_DEVICE_OVERFLOW;
            rx_state.gap_samples =
                    rx_state.have_time && h.time >= rx_state.next_time ? int64_t(h.time - rx_state.next_time) : -1;
            ++dev->stats.overflows;
        } else if (rx_state.have_time)
            rx_state.flags_pending |= OCH_META_DISCONTINUITY;
        if (h.flags & frame_rx_late) rx_state.flags_pending |= OCH_META_LATE;
        if (rx_state.reconfigured_rx) {
            rx_state.flags_pending |= OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY;
            rx_state.reconfigured_rx = false;
        }
    }
    if (rx_state.retuned.exchange(false)) {
        publish_rx();
        rx_state.flags_pending |= OCH_META_RECONFIGURED | OCH_META_DISCONTINUITY;
    }
    rx_state.have_time = true;
    rx_state.cur_time = h.time;
    rx_state.next_time = h.time + n / group;
    const bool eob = (h.flags & frame_rx_eob) != 0;
    if (!n && eob && !rx_state.rx_filled) {
        // empty end-of-burst marker block
        auto b = ring.write();
        if (b) {
            och_metadata m{};
            m.struct_size = sizeof m;
            m.flags = rx_state.flags_pending | OCH_META_END_OF_BURST | OCH_META_HAS_TIME;
            m.epoch = rx_state.epoch;
            m.sample_index = rx_state.index;
            m.time_ticks = rx_state.cur_time;
            m.tick_rate = dev->config.sample_rate;
            m.host_monotonic_ns = now_ns();
            rx_state.flags_pending = 0;
            ring.publish(0, m);
        }
        return;
    }
    while (n) {
        auto b = ring.write();
        if (!b) {
            auto lost = n / group;
            stats.host_dropped += lost;
            rx_state.dropped_pending += lost;
            rx_state.index += lost;
            rx_state.cur_time += lost;
            rx_state.flags_pending |= OCH_META_HOST_OVERFLOW | OCH_META_DISCONTINUITY;
            auto now = now_ns();
            if (now - rx_state.last_drop_notice >= 500000000ULL) {
                dev->emit(OCH_EVENT_HOST_OVERFLOW, "RX host ring full; new samples dropped (see stats)",
                          rx_state.epoch);
                rx_state.last_drop_notice = now;
            }
            return;
        }
        if (!rx_state.rx_filled) {
            rx_state.rx_meta = {};
            rx_state.rx_meta.struct_size = sizeof rx_state.rx_meta;
            rx_state.rx_meta.flags = rx_state.flags_pending | OCH_META_HAS_TIME;
            rx_state.rx_meta.epoch = rx_state.epoch;
            rx_state.rx_meta.sample_index = rx_state.index;
            rx_state.rx_meta.host_dropped_samples = rx_state.dropped_pending;
            rx_state.rx_meta.device_dropped_samples =
                    (rx_state.flags_pending & OCH_META_DEVICE_OVERFLOW) ? rx_state.gap_samples : 0;
            rx_state.rx_meta.time_ticks = rx_state.cur_time;
            rx_state.rx_meta.tick_rate = dev->config.sample_rate;
            rx_state.rx_meta.host_monotonic_ns = now_ns();
            rx_state.flags_pending = 0;
            rx_state.dropped_pending = 0;
        }
        size_t take = std::min(n, args.transfer_bytes - rx_state.rx_filled);
        take -= take % group;
        memcpy(b->data.get() + rx_state.rx_filled, p, take);
        rx_state.rx_filled += take;
        p += take;
        n -= take;
        rx_state.index += take / group;
        rx_state.cur_time += take / group;
        if (rx_state.rx_filled == args.transfer_bytes) publish_rx();
    }
    if (eob && rx_state.rx_filled) {
        rx_state.rx_meta.flags |= OCH_META_END_OF_BURST;
        publish_rx();
    }
}

// TX block builder. The open block lives directly in the USB slot at
// blk_start; slot->used only advances over finished 16 KiB blocks.
void Stream::tx_open(Slot *s, uint32_t f, uint64_t t) {
    tx_state.blk_open = true;
    tx_state.blk_start = s->used;
    tx_state.blk_used = 0;
    tx_state.blk_time = 0;
    tx_state.blk_flags = 0;
    if (tx_state.tx_first || (f & OCH_TX_START_OF_BURST)) tx_state.blk_flags |= och::dsp::frame_tx_sob;
    if (f & OCH_TX_HAS_TIME) {
        tx_state.blk_flags |= och::dsp::frame_tx_has_time | och::dsp::frame_tx_sob;
        tx_state.blk_time = t;
    }
    tx_state.tx_first = false;
}

void Stream::tx_close(Slot *s, uint8_t extra) {
    using namespace och::dsp;
    uint8_t *b = s->bytes.get() + tx_state.blk_start;
    memset(b + frame_header_bytes + tx_state.blk_used, 0, frame_payload_bytes - tx_state.blk_used);
    FrameHeader h;
    h.flags = uint8_t(tx_state.blk_flags | extra);
    h.seq = tx_state.tx_seq++;
    h.valid = uint16_t(tx_state.blk_used / group);
    h.time = tx_state.blk_time;
    write_frame_header(b, h);
    stats.padding += (frame_payload_bytes - tx_state.blk_used) / group;
    s->used = tx_state.blk_start + frame_block_bytes;
    tx_state.blk_open = false;
    tx_state.blk_used = 0;
}

bool Stream::fill_tx(Slot *s) {
    using namespace och::dsp;
    for (;;) {
        if (!tx_state.blk_open && s->used >= args.transfer_bytes) {
            assembling = s->used;
            return true;
        }
        auto b = ring.read();
        if (!b) {
            if (flushing && tx_state.blk_open) {
                tx_close(s, frame_tx_eob);
                assembling = 0;
                return true;
            }
            if (flushing && s->used) {
                assembling = 0;
                return true;
            }
            assembling = s->used + (tx_state.blk_open ? tx_state.blk_used : 0);
            return false;
        }
        if (tx_state.tx_offset == 0 && tx_state.blk_open && tx_state.blk_used &&
            (b->meta.flags & (OCH_TX_START_OF_BURST | OCH_TX_HAS_TIME))) {
            tx_close(s, 0);
            continue; // a new burst always starts a new block
        }
        if (!tx_state.blk_open) {
            if (s->used >= args.transfer_bytes) {
                assembling = s->used;
                return true;
            }
            tx_open(s, tx_state.tx_offset == 0 ? b->meta.flags : 0, b->meta.time_ticks);
        } else if (tx_state.tx_offset == 0 && !tx_state.blk_used &&
                   (b->meta.flags & (OCH_TX_START_OF_BURST | OCH_TX_HAS_TIME))) {
            tx_state.blk_open = false;
            tx_open(s, b->meta.flags, b->meta.time_ticks);
        }
        const size_t n = std::min(b->size - tx_state.tx_offset, frame_payload_bytes - tx_state.blk_used);
        memcpy(s->bytes.get() + tx_state.blk_start + frame_header_bytes + tx_state.blk_used,
               b->data.get() + tx_state.tx_offset, n);
        tx_state.blk_used += n;
        tx_state.tx_offset += n;
        const bool done = tx_state.tx_offset == b->size;
        const bool eob = done && (b->meta.flags & OCH_TX_END_OF_BURST);
        if (done) {
            ring.release();
            tx_state.tx_offset = 0;
        }
        if (eob) {
            tx_close(s, frame_tx_eob);
            tx_state.tx_first = false;
            assembling = 0;
            return true;
        }
        if (tx_state.blk_used == frame_payload_bytes) tx_close(s, 0);
    }
}

void Stream::publish_rx() {
    if (!rx_state.rx_filled) return;
    ring.publish(rx_state.rx_filled, rx_state.rx_meta);
    rx_state.rx_filled = 0;
    auto bytes = ring.queued();
    auto prev = stats.peak.load();
    if (bytes > prev) stats.peak = bytes;
}

bool Stream::submit(Slot *s) {
    auto &u = dev->usb;
    UCHAR ep = args.direction == OCH_RX ? 0x81 : 0x01;
    if (args.direction == OCH_RX) s->used = xfer_bytes;
    ResetEvent(s->ov.hEvent);
    BOOL ok = args.direction == OCH_RX
                      ? u.api.read(u.h, ep, s->bytes.get(), static_cast<ULONG>(s->used), nullptr, &s->ov)
                      : u.api.write(u.h, ep, s->bytes.get(), static_cast<ULONG>(s->used), nullptr, &s->ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        throw Failure(OCH_IO, "USB streaming submission failed: " + utf8(syserr(GetLastError())));
    if (ok) SetEvent(s->ov.hEvent);
    s->pending = true;
    active.push_back(s);
    pending = active.size();
    assembling = 0;
    return true;
}

void Stream::cancel_all() {
    auto &u = dev->usb;
    UCHAR ep = args.direction == OCH_RX ? 0x81 : 0x01;
    if (!active.empty()) {
        u.api.abort(u.h, ep);
        while (!active.empty()) {
            auto *s = active.front();
            active.pop_front();
            if (args.direction == OCH_TX) cancelled_bytes += s->used;
#ifdef _WIN32
            CancelIoEx(u.file, &s->ov);
#endif
            ULONG n = 0;
            u.api.result(u.h, &s->ov, &n, TRUE);
            s->pending = false;
        }
        active.clear();
        pending = 0;
    }
    UCHAR off = FALSE;
    u.api.setPolicy(u.h, ep, RAW_IO, sizeof off, &off);
}

void Stream::run() {
    auto &u = dev->usb;
    UCHAR ep = args.direction == OCH_RX ? 0x81 : 0x01;
    och::core::FixedQueue<Slot *, 128> free;
    for (auto &s: slots) free.push_back(s.get());
    bool starved_notice = false;
    try {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        require(args.transfer_bytes <= u.maximum(ep), OCH_INVALID, "USB transfer block exceeds driver maximum");
        u.timeout(ep, 0);
        u.policy(ep, RAW_IO, FALSE);
        if (args.direction == OCH_RX) {
            u.policy(ep, IGNORE_SHORT_PACKETS, FALSE);
            u.policy(ep, ALLOW_PARTIAL_READS, TRUE);
            u.policy(ep, AUTO_FLUSH, FALSE);
        } else {
            u.policy(ep, SHORT_PACKET_TERMINATE, FALSE);
            u.parallelLimit(ep, args.usb_queue_depth);
        }
        u.policy(ep, RAW_IO, TRUE);
        while (!stop_requested) {
            while (!free.empty() && !stop_requested) {
                Slot *s = free.front();
                if (args.direction == OCH_TX && !fill_tx(s)) break;
                free.pop_front();
                submit(s);
                starved_notice = false;
            }
            queued = true;
            if (active.empty()) {
                if (args.direction == OCH_TX && started && !flushing && !starved_notice) {
                    ++stats.starved;
                    dev->emit(OCH_EVENT_TX_STARVED, "TX application queue empty; producer must resume");
                    starved_notice = true;
                }
                ring.wait_read(2, stop_requested);
                continue;
            }
            Slot *s = active.front();
            HANDLE events[2] = {stop_event.h, s->ov.hEvent};
            DWORD w = WaitForMultipleObjects(2, events, FALSE, 2);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_TIMEOUT) continue;
            require(w == WAIT_OBJECT_0 + 1, OCH_IO, "USB event wait failed");
            ULONG n = 0;
            const auto completed = u.api.result(u.h, &s->ov, &n, FALSE);
            require(completed != FALSE, OCH_IO, "USB transfer failed: " + utf8(syserr(GetLastError())));
            require(n <= s->used, OCH_IO, "driver reported invalid USB completion length");
            if (args.direction == OCH_TX)
                require(n == s->used, OCH_IO, "short TX completion; byte order cannot be retried safely");
            active.pop_front();
            s->pending = false;
            pending = active.size();
            stats.usb_bytes += n;
            if (args.direction == OCH_RX) {
                rx_state.fparser.feed(s->bytes.get(), n, &Stream::frame_sink, this);
                stats.headers = rx_state.fparser.blocks();
                stats.resyncs = rx_state.fparser.resyncs();
            }
            s->used = 0;
            free.push_back(s);
        }
    } catch (const std::exception &e) {
        fail(e.what());
    }
    cancel_all();
    ended_ns = now_ns();
    running = false;
    stop_requested = true;
    ring.wake();
}

void Stream::start() {
    require(!ever_started && !stop_requested, OCH_BUSY,
            "stream is single-use; destroy and create a new stream after stop");
    require(dev->configured, OCH_INVALID, "configure the device before starting streams");
    if (dev->transport_dirty) {
        require(!dev->rx_on && !dev->tx_on, OCH_BUSY,
                "stop both directions before reopening a stream (CH32 buffer cleanup)");
        dev->reset_transport();
    }
    if (args.direction == OCH_TX) require(ring.queued() > 0, OCH_BUSY, "queue TX samples before start");
    if (args.direction == OCH_RX)
        dev->command(std::string("rx_timed=") + ((args.start_flags & OCH_RX_TIMED) ? "1" : "0"));
    if (args.direction == OCH_TX) {
        dev->command("usb_data_en=" + std::to_string(dev->rx_on ? 1 : 0) + " 1");
        dev->tx_on = true;
    }
    // Producers may already be waiting on a full prefill ring. Publish running
    // first so they cannot mistake the startup transition for a stopped worker.
    running = true;
    ever_started = true;
    if (!started_ns) started_ns = now_ns();
    try {
        worker = std::thread([this] { run(); });
        auto deadline = now_ns() + 5000000000ULL;
        while (!queued && running && now_ns() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(queued && running, OCH_IO, "USB queue startup failed: " + error());
        if (args.direction == OCH_RX) {
            dev->command("usb_data_en=1 " + std::to_string(dev->tx_on ? 1 : 0));
            dev->rx_on = true;
            if ((args.start_flags & OCH_RX_TIMED) &&
                (args.start_flags & OCH_RX_NOW || args.start_time_lo || args.start_time_hi)) {
                const uint64_t t = uint64_t(args.start_time_hi) << 32 | args.start_time_lo;
                dev->command("rx_stream=" + ((args.start_flags & OCH_RX_NOW) ? std::string("now") : std::to_string(t)) +
                             " " + std::to_string(args.num_samples));
            }
        } else {
            // IQ8's first 16 KiB packet fills about 2046 raw FIFO groups.
            // Waiting for the legacy 2700 blocks the second packet before
            // TX_RUN drains the FIFO. Match the SDK's IQ8 first-packet gate.
            // Framed: one complete block (1364 or 2046 groups) proves the
            // first burst block reached the FPGA; the header controls timing.
            // The DAC prefetch cache already holds up to 4 frames (1-2 groups);
            // a single-channel FIFO admits block two only after half of block
            // one played, so the count can stay just below a whole block.
            bool filled = false;
            unsigned threshold = unsigned(frame_groups()) - 16;
            while (now_ns() < deadline && running) {
                auto reply = dev->command("usb_status?");
                auto i = reply.find("tx_main_fifo_groups=");
                if (i != std::string::npos) {
                    unsigned groups = 0;
                    auto first = reply.c_str() + i + 20;
                    if (sscanf(first, "%u", &groups) == 1 && groups >= threshold) {
                        filled = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            require(filled, OCH_TIMEOUT, "TX prefill did not reach FPGA threshold: " + error());
            dev->command("tx_enable=1");
        }
        require(running && !stop_requested, OCH_IO, "USB worker stopped during startup: " + error());
        started = true;
    } catch (...) {
        try {
            if (args.direction == OCH_RX)
                dev->command("usb_data_en=0 " + std::to_string(dev->tx_on ? 1 : 0));
            else
                dev->command("tx_enable=0");
        } catch (...) {}
        if (args.direction == OCH_RX)
            dev->rx_on = false;
        else
            dev->tx_on = false;
        stop_requested = true;
        SetEvent(stop_event.h);
        ring.wake();
        if (worker.joinable()) worker.join();
        running = false;
        ended_ns = now_ns();
        dev->transport_dirty = true;
        throw;
    }
}

void Stream::flush(uint32_t timeout) {
    // Held for the whole drain: a concurrent och_send fails try_lock and returns OCH_TIMEOUT.
    std::unique_lock<std::mutex> io(io_mutex, std::try_to_lock);
    require(io.owns_lock(), OCH_BUSY, "TX IO in progress");
    require(!reconfiguring, OCH_TIMEOUT, "TX flush interrupted by reconfiguration");
    require(args.direction == OCH_TX && started, OCH_INVALID, "flush requires a started TX stream");
    require(running && !stop_requested, OCH_IO, "TX worker stopped before flush: " + error());
    require(!flushing.exchange(true), OCH_BUSY, "TX flush already in progress");
    auto until = now_ns() + uint64_t(timeout) * 1000000;
    try {
        while (ring.queued() || assembling || pending) {
            require(!reconfiguring, OCH_TIMEOUT, "TX flush interrupted by reconfiguration");
            require(running && !stop_requested, OCH_IO, "TX worker stopped: " + error());
            require(now_ns() < until, OCH_TIMEOUT, "TX flush timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(!reconfiguring, OCH_TIMEOUT, "TX flush interrupted by reconfiguration");
        require(running && !stop_requested, OCH_IO, "TX flush was interrupted: " + error());
    } catch (...) {
        flushing = false;
        throw;
    }
    flushing = false;
}

void Stream::stop(uint32_t timeout) {
    if (!ever_started) {
        stop_requested = true;
        ring.wake();
        return;
    }
    std::string issue;
    try {
        if (args.direction == OCH_RX && dev->rx_on) {
            dev->command("usb_data_en=0 " + std::to_string(dev->tx_on ? 1 : 0), timeout ? timeout : 3000);
            dev->rx_on = false;
        }
        if (args.direction == OCH_TX && dev->tx_on) {
            dev->command("tx_enable=0", timeout ? timeout : 3000);
            dev->tx_on = false;
        }
    } catch (const std::exception &e) {
        issue = e.what();
    }
    halt();
    dev->transport_dirty = true;
    if (!issue.empty()) throw Failure(OCH_IO, "stop completed with an error: " + issue);
}

void Stream::halt() {
    started = false;
    stop_requested = true;
    SetEvent(stop_event.h);
    ring.wake();
    if (worker.joinable()) worker.join();
    running = false;
    if (started_ns) ended_ns = now_ns();
}

// Both the USB worker and the application IO endpoint are quiescent. Large
// ring/USB allocations and the public stream pointer deliberately survive.
void Stream::prepare_restart(bool receive_gap) {
    rx_state.epoch_base += rx_state.fr_epochs;
    rx_state.fparser.reset();
    rx_state.fr_epochs = 0;
    rx_state.have_time = false;
    tx_state.blk_open = false;
    tx_state.blk_used = 0;
    tx_state.tx_first = true;
    rx_state.reconfigured_rx = rx_state.reconfigured_rx || (args.direction == OCH_RX && receive_gap);
    ring.reset();
    reader.recv_offset = rx_state.rx_filled = tx_state.tx_offset = 0;
    assembling = 0;
    rx_state.rx_meta = {};
    active.clear();
    pending = 0;
    for (auto &s: slots) {
        s->used = 0;
        s->pending = false;
        s->ov = {};
        s->ov.hEvent = s->event.h;
        ResetEvent(s->event.h);
    }
    ResetEvent(stop_event.h);
    queued = false;
    started = false;
    running = false;
    flushing = false;
    ever_started = false;
    stop_requested = false;
}

ResumePlan Stream::snapshot_for_reconfigure() const {
    ResumePlan plan;
    plan.resume = started && running && !stop_requested;
    plan.stopped = stop_requested;
    plan.ever_started = ever_started;
    return plan;
}

uint64_t Stream::halt_for_reconfigure() {
    const auto before = cancelled_bytes.load();
    halt();
    return cancelled_bytes.load() - before;
}

uint64_t Stream::rx_unread_samples() const { return (ring.queued() - reader.recv_offset + rx_state.rx_filled) / group; }

uint64_t Stream::discard_for_reconfigure(const ResumePlan &plan) {
    uint64_t discarded = 0;
    if (args.direction == OCH_RX) {
        discarded = rx_unread_samples();
        stats.host_dropped += discarded;
        rx_state.dropped_pending += discarded;
    } else {
        // tx_offset is already copied out of the first ring block. Pending
        // USB transfers are uncertain delivery, not replayable samples.
        discarded = (ring.queued() - tx_state.tx_offset + assembling.load()) / group;
    }
    prepare_restart(plan.resume || plan.ever_started);
    if (plan.resume) {
        std::lock_guard<std::mutex> lock(failure_mutex);
        failure.clear();
    }
    // prepare_restart() cleared stop_requested and ever_started: a stream that
    // was already stopped before the reconfiguration stays stopped.
    if (plan.stopped && !plan.resume) {
        stop_requested = true;
        ever_started = plan.ever_started;
        SetEvent(stop_event.h);
    }
    return discarded;
}

uint64_t Stream::prefill_zero(unsigned blocks) {
    for (unsigned i = 0; i < blocks; ++i) {
        auto *b = ring.write();
        require(b != nullptr, OCH_IO, "TX restart prefill capacity");
        memset(b->data.get(), 0, args.transfer_bytes);
        ring.publish(args.transfer_bytes, {});
    }
    const uint64_t zero_samples = uint64_t(blocks) * args.transfer_bytes / group;
    stats.padding += zero_samples;
    return zero_samples;
}

void Stream::fail_reconfigure() {
    halt();
    // No stale data may look like a successful capture after failure.
    if (args.direction == OCH_RX) {
        const auto lost = rx_unread_samples();
        stats.host_dropped += lost;
        rx_state.dropped_pending += lost;
    }
    ring.reset();
    reader.recv_offset = rx_state.rx_filled = tx_state.tx_offset = 0;
    assembling = 0;
    std::lock_guard<std::mutex> lock(failure_mutex);
    failure = "reconfiguration failed; configure and recreate streams";
}
