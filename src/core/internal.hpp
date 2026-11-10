#pragma once
#include <omnicore/och.h>
#include "ring.hpp"
#include "power_status.hpp"
#include "../transport/link.hpp"
#include "../dsp/codec.hpp"
#include "../protocol/frame.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace och::core {
    // Implementation classes behind the C handles och_device / och_stream (defined below).
    struct Device;
    struct Stream;

    struct Failure : std::runtime_error {
        int code;

        Failure(int c, const std::string &s) : std::runtime_error(s), code(c) {}
    };

    inline void require(bool ok, int code, const std::string &why) {
        if (!ok) throw Failure(code, why);
    }

    inline uint64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
    }

    inline uint32_t channels(uint32_t mask) { return mask == 3 ? 2 : 1; }
    inline uint32_t iq_bits(const och_config &c) { return c.usb_iq_bits ? c.usb_iq_bits : 12; }

    // Bytes of one complex sample in an application format (och_sample_size); 0 if unknown.
    inline size_t sample_size(uint32_t format) {
        return format == OCH_PACKED12 ? 3 : format == OCH_SC16 ? 4 : format == OCH_FC32 ? 8 : format == OCH_SC8 ? 2 : 0;
    }

    // The configuration och_default_config returns.
    och_config default_config();

    inline void copy_text(char *to, size_t n, const std::string &s) {
        if (n) {
            size_t k = std::min(n - 1, s.size());
            memcpy(to, s.data(), k);
            to[k] = 0;
        }
    }

    // Link::transfer with a transport error reported as OCH_IO: "<channel> submit: ..." or
    // "<channel> completion: ...".
    inline size_t bounded_transfer(och::transport::Link &link, och::transport::Pipe pipe, uint8_t *data, size_t size,
                                   uint32_t timeout_ms, const char *channel) {
        try {
            return link.transfer(pipe, data, size, timeout_ms);
        } catch (const och::transport::Error &e) {
            throw Failure(OCH_IO, std::string(channel) + " " + e.what());
        }
    }

    template<class T, size_t N>
    class FixedQueue {
        std::array<T, N> values{};
        size_t first = 0, count = 0;

    public:
        bool empty() const { return count == 0; }
        size_t size() const { return count; }
        T front() const { return values[first]; }

        void push_back(T p) {
            if (count == N) throw std::logic_error("fixed USB queue overflow");
            values[(first + count) % N] = p;
            ++count;
        }

        void pop_front() {
            first = (first + 1) % N;
            --count;
        }

        void clear() { first = count = 0; }
    };

    void fill_info(och_device_info &, const och::transport::DeviceId &);

    // CH32 bridge firmware (firmware.cpp). firmware_probe is false for bridge
    // firmware without the OCFW requests (legacy).
    bool firmware_probe(och::transport::Link &, och_firmware_info &, const std::string &serial);

    och_firmware_image firmware_read_image(const char *path);

    och_firmware_info firmware_query(const char *serial);

    void firmware_enter_bootloader(const char *serial);

    void firmware_start_app(const char *serial);

    void firmware_update(const char *serial, const char *path, och_firmware_progress, void *user);

    // EVT lines from the SDK time module; true when consumed (timebase.cpp).
    bool timed_event(Device &, const std::string &line);

    // Multi-board sync (sync.cpp). Callers hold the device's state_mutex except
    // for multi_sync, which takes each device's lock per transaction.
    och_sync_status read_sync_status(Device &);

    void apply_sync_config(Device &, const och_sync_config &);

    void sync_trigger(Device &, uint32_t mode);

    void multi_sync(Device *const *devices, size_t count, const och_multi_sync_args &, och_multi_sync_result *results);

    // Bounded device event queue. push() runs on any thread (control, USB worker,
    // configuration); pop() is och_poll_event on the application thread. The newest
    // `capacity` events are kept: a push into a full queue drops the oldest one.
    class EventQueue {
    public:
        static constexpr size_t capacity = 256;

        // Assigns the sequence number and wakes one waiting pop(), under the lock.
        void push(och_event e);

        // Waits up to timeout_ms for an event; false on timeout.
        bool pop(och_event &out, uint32_t timeout_ms);

        // Events dropped because the queue was full (och_stats.event_drops).
        uint64_t drops() const { return dropped; }

    private:
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<och_event> queue;
        std::atomic<uint64_t> seq{0}, dropped{0};
    };

    // SDK command channel of one device. A reader thread splits Pipe::control_in into lines:
    // "EVT ..." lines go to on_event, the rest belongs to the reply of the pending command.
    // command() runs on any thread, one at a time (command_mutex); the reply state is shared
    // with the reader under reply_mutex. After a timeout, a broken reply or loss of the pipe the
    // channel stays failed until the device is reopened.
    class ControlChannel {
    public:
        ~ControlChannel();

        // Starts the reader on an opened link. Both callbacks run on the reader thread;
        // disconnect reports why the pipe was lost.
        void start(och::transport::Link &, std::function<void(const std::string &)> event,
                   std::function<void(const std::string &)> disconnect);

        // Ends the reader: stop flag, Link::shutdown_control(), join. Safe to repeat.
        void stop();

        // Sends one SDK command and returns its reply text; throws Failure.
        std::string command(const std::string &, uint32_t timeout);

    private:
        void loop();

        void line(const std::string &);

        och::transport::Link *link = nullptr;
        std::function<void(const std::string &)> on_event, on_disconnect;
        std::atomic<bool> stopping{false};
        std::thread reader;
        std::mutex command_mutex, reply_mutex;
        std::condition_variable reply_cv;
        uint32_t next_id = 0, pending_id = 0;
        bool reply_started = false, reply_done = false, control_failed = false;
        int reply_result = 0;
        std::string reply, control_error;
    };
}

struct och::core::Device {
    // The opened device. Declared before owner, so the cross-process lock is released first.
    std::unique_ptr<och::transport::Link> link;
    och::transport::DeviceLock owner;
    och_device_info info{};
    och_config config{};
    bool configured = false, transport_dirty = false;
    och_firmware_info firmware{}; // CH32 bridge, read at open
    std::mutex state_mutex;
    // state_mutex serializes control transactions; IO gates protect buffers
    // while a transaction reuses the existing public stream handles.
    bool reconfiguring_control = false;
    Stream *rx = nullptr, *tx = nullptr;
    bool rx_on = false, tx_on = false;
    ControlChannel control;
    EventQueue events;
    // Device FIFO faults from EVT lines (control thread); read by och_get_stats.
    struct DeviceStats {
        std::atomic<uint64_t> overflows{0}, underflows{0};
    } stats;
    och::core::PowerLogParser power_log;

    ~Device();

    // Takes the cross-process device lock, opens the device and attaches its link.
    void open(const och::transport::DeviceId &);

    // Handshake on an opened link: bridge firmware, bus speed, SDK protocol, transport reset.
    void attach(std::unique_ptr<och::transport::Link>);

    std::string command(const std::string &, uint32_t timeout = 5000);

    // One "EVT ..." line from the SDK, on the control channel's reader thread.
    void event_line(const std::string &);

    void emit(uint32_t type, const std::string &, uint64_t epoch = 0, uint64_t hw_time = 0, uint32_t code = 0,
              uint32_t block = 0, uint32_t count = 1);

    void reset_transport();

    // Stream slots rx/tx: changed only under state_mutex; destroy order is stop, clear slot, free.
    // Handle is the type the stream is allocated as (och_stream in src/capi/), so new and delete
    // both see the real type. Definitions follow Stream below.
    template<class Handle>
    Handle *create_stream(const och_stream_args &);

    template<class Handle>
    void destroy_stream(Handle *);

    // Device state a new stream needs (configured, sc8 needs 8-bit IQ, one stream per direction,
    // direction has channels). Caller holds state_mutex.
    void check_stream_slot(const och_stream_args &);

    // Lease check, stop and slot clear before the stream is freed. Caller holds state_mutex.
    // Returns the stop failure text, empty on success.
    std::string detach_stream(Stream &);

    void configure(const och_config &);

    void apply_config(const och_config &);

    // LO, manual gain and attenuation changes that keep IQ streams running.
    void retune(const och_config &);

    bool tune_lo(const och_config &, const och_config &before, bool full);

    void tune_gain(const och_config &, const och_config &before, bool full, bool gain_table_changed);

    void reconfigure(const std::function<void()> &);

    och_fir_config get_fir(unsigned direction);

    void set_fir(const och_fir_config &);

    void enable_fir(unsigned rx_enabled, unsigned tx_enabled);

    och_clock_config get_clock();

    void set_clock(const och_clock_config &);

    void set_sample_rate(uint32_t rate);

    // Applies rate or, if the SDK has no exact clock plan for it, the nearest
    // accepted rate; returns the applied rate.
    uint32_t apply_sample_rate(uint32_t rate);

    uint32_t requested_sample_rate = 0; // last request mapped to config.sample_rate
    och_rx_correction_config get_rx_correction();

    void set_rx_correction(bool dc, unsigned enabled);

    och_lo_source_config get_lo_source();

    void set_lo_source(bool receive, unsigned source);

    och_reference_status get_reference_status();

    void set_reference_source(unsigned source);

    och_reference_test_result test_reference();

    och_pps_status get_pps_status();

    void set_pps_enabled(unsigned enabled);

    och_vcxo_status get_vcxo_status();

    och_rgb_status get_rgb_status();

    void set_rgb_enabled(unsigned enabled);

    och_fan_status get_fan_status();

    void set_fan_enabled(unsigned enabled);

    och_core_temperature get_core_temperature();

    och_rx_rssi get_rx_rssi();

    void set_rx_rssi_enabled(unsigned enabled);

    // system_info? from open; capabilities are fixed for a firmware image.
    std::string capabilities;
    // Cached LO selection: bit0 RX external, bit1 TX external; -1 unknown.
    int lo_sources = -1;

    bool supports_capability(const char *);

    void require_capability(const char *);

    och_power_status get_power_status(uint32_t timeout_ms);

    och_time_status get_time();
};

namespace och::core {
    // Stream state taken before a coordinated reconfiguration changes anything.
    struct ResumePlan {
        bool resume = false;       // running when the reconfiguration began: restart it afterwards
        bool stopped = false;      // och_stop was already requested: keep it stopped
        bool ever_started = false; // keeps the och_stop / och_send semantics of a once-started stream
    };
} // namespace och::core

struct och::core::Stream {
    using Slot = och::transport::Transfer;

    // Threads: control = start/stop/halt/prepare_restart and reconfiguration, under the device's
    // state_mutex; worker = run() and what it calls (rx_frame, publish_rx, fill_tx, submit, ...);
    // application = recv / rx_acquire / rx_release / send / flush, under io_mutex.

    // Fixed at construction, then read-only.
    Device *dev;
    och_stream_args args;
    unsigned nchan, wire_bytes, group;
    // Timed framed RX: one 16 KiB device block per USB transfer, so a finite
    // burst's last block (and its END_OF_BURST) completes without a short packet.
    size_t xfer_bytes = 0;

    // Host queue between worker and application; it synchronises itself. Control resets it
    // only while the worker is stopped and application IO is paused.
    och::core::Ring ring;

    // Lifecycle. Written by control (and running / ended_ns by the worker as it exits);
    // atomics read by all three. failure is guarded by failure_mutex: the worker sets it
    // through fail(), anyone reads it through error().
    std::thread worker;
    std::atomic<bool> stop_requested{false}, running{false}, queued{false}, started{false}, flushing{false};
    std::atomic<bool> ever_started{false};
    std::atomic<uint64_t> started_ns{0}, ended_ns{0};
    std::mutex failure_mutex;
    std::string failure;

    // USB transfers of this direction: the worker's, while it runs; control resets the queue
    // after halt(). queue->wake() is how every thread asks the worker to leave its wait.
    // pending (transfers in flight) is written by the worker only; pending / assembling are
    // atomic because flush() and fill_stats() read them.
    std::unique_ptr<och::transport::SampleQueue> queue;
    std::atomic<uint64_t> pending{0};
    std::atomic<size_t> assembling{0};
    std::atomic<uint64_t> cancelled_bytes{0};

    // IO gate. io_mutex: held by each application IO call and by IoPause during a
    // reconfiguration; reconfiguring is true whenever IoPause holds it (see reconfigure.cpp).
    std::mutex io_mutex;
    std::atomic<bool> reconfiguring{false};

    // RX framing, worker side (rx_frame / publish_rx); moves into RxAssembler in 2.6a.
    // Worker only while running; control resets it in prepare_restart() while stopped.
    // Exceptions: retuned is set by control (running retune) and consumed by the worker;
    // epoch_base and dropped_pending are also used by control while the worker is stopped;
    // epoch is read by the control thread in timebase.cpp without synchronisation, a known
    // race (see REFACTOR_PLAN.md reading notes).
    struct RxState {
        explicit RxState(size_t frame_bytes) : fparser(frame_bytes) {}
        // Framed transport: 16-byte header per 16 KiB block (the only transport).
        och::dsp::FrameParser fparser;
        bool have_time = false;
        uint64_t next_time = 0, cur_time = 0, fr_epochs = 0;
        int64_t gap_samples = 0;
        uint64_t epoch = 0, epoch_base = 0, index = 0, dropped_pending = 0, last_drop_notice = 0;
        uint32_t flags_pending = 0;
        bool reconfigured_rx = false;
        // Set after a running-stream retune; the next RX block starts a new segment.
        std::atomic<bool> retuned{false};
        // RX producer partial output block, always one epoch per block.
        size_t rx_filled = 0;
        och_metadata rx_meta{};
    } rx_state;

    // RX read position, application side: under io_mutex. Not part of the framer.
    // lease is also read under state_mutex alone (configure, Device::destroy_stream).
    struct RxRead {
        size_t recv_offset = 0;
        bool lease = false;
    } reader;

    // TX block assembly, worker side (fill_tx / tx_open / tx_close); moves into TxAssembler
    // in 2.6a. Worker only while running; control resets it while stopped.
    struct TxState {
        bool blk_open = false, tx_first = true;
        size_t blk_start = 0, blk_used = 0;
        uint8_t blk_flags = 0;
        uint64_t blk_time = 0;
        uint16_t tx_seq = 0;
        // TX consumer partial input block.
        size_t tx_offset = 0;
    } tx_state;

    // Counters for och_get_stats. Atomic: written by the worker, the application (samples,
    // peak) or control (host_dropped, padding), read by fill_stats().
    struct StreamStats {
        std::atomic<uint64_t> usb_bytes{0}, samples{0}, host_dropped{0}, headers{0}, resyncs{0}, peak{0}, errors{0},
                starved{0}, padding{0};
    } stats;

    Stream(Device *, const och_stream_args &);

    ~Stream();

    void start();

    void stop(uint32_t timeout_ms);

    void flush(uint32_t timeout_ms);

    void run();

    void halt();

    void prepare_restart(bool receive_gap);

    // Coordinated reconfiguration (Device::reconfigure), in call order. The caller holds
    // state_mutex and, through IoPause, this stream's io_mutex.
    och::core::ResumePlan snapshot_for_reconfigure() const;

    // halt() and return the USB bytes it cancelled.
    uint64_t halt_for_reconfigure();

    // Account the queued samples that cannot survive and reset for a restart; returns that count.
    uint64_t discard_for_reconfigure(const och::core::ResumePlan &);

    // TX only: queue zero blocks so a resumed TX does not underflow at start; returns their samples.
    uint64_t prefill_zero(unsigned blocks);

    // Failure path: stop, count unread RX samples as lost and mark the stream unusable.
    void fail_reconfigure();

    // RX samples still in the ring or the partial output block, not yet returned to the application.
    uint64_t rx_unread_samples() const;

    void fail(const std::string &);

    std::string error();

    bool submit(Slot *);

    void cancel_all();

    void publish_rx();

    void rx_frame(const och::dsp::FrameHeader &, const uint8_t *, size_t);

    static void frame_sink(void *, const och::dsp::FrameHeader &, const uint8_t *, size_t);

    bool fill_tx(Slot *);

    void tx_open(Slot *, uint32_t flags, uint64_t time);

    void tx_close(Slot *, uint8_t extra);

    size_t frame_groups() const { return wire_bytes == 2 ? 2046 : 1364; }

    // Application-thread side (stream_io.cpp), called by the C API after argument checks.
    // Return OCH_OK / OCH_TIMEOUT / OCH_STOPPED, or throw Failure.
    int recv(void *output, size_t capacity, size_t *received, och_metadata *, uint32_t timeout_ms);

    int rx_acquire(const void **data, size_t *bytes, och_metadata *, uint32_t timeout_ms);

    void rx_release();

    // flags/time come from och_tx_metadata (0 for och_send).
    int send(const void *data, size_t count, uint32_t flags, uint64_t time, size_t *accepted, uint32_t timeout_ms);

    void fill_stats(och_stats *);

    int await_rx(uint32_t timeout_ms);

    // Metadata of the current RX block as seen from recv_offset (a read that resumes mid-block).
    och_metadata adjusted(const och::core::Ring::Block &) const;
};

template<class Handle>
Handle *och::core::Device::create_stream(const och_stream_args &a) {
    std::lock_guard<std::mutex> lock(state_mutex);
    check_stream_slot(a);
    auto *handle = new Handle(this, a);
    (a.direction == OCH_RX ? rx : tx) = handle;
    return handle;
}

template<class Handle>
void och::core::Device::destroy_stream(Handle *s) {
    std::lock_guard<std::mutex> lock(state_mutex);
    const std::string issue = detach_stream(*s);
    delete s;
    if (!issue.empty()) throw Failure(OCH_IO, issue);
}

// The public opaque handles (och.h). They add nothing: core code works on Device / Stream and
// only src/capi/ uses these names. Every Stream is created as an och_stream.
struct och_device final : och::core::Device {};

struct och_stream final : och::core::Stream {
    using Stream::Stream;
};
