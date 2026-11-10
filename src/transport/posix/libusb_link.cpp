// SPDX-License-Identifier: GPL-3.0-or-later
// Linux and macOS backend of the transport interface (link.hpp): libusb-1.0 asynchronous
// transfers. One event thread per opened device runs the libusb completion callbacks; each
// SampleQueue has its own lock and condition variable, so streams and devices do not wake each
// other. The cross-process lock is flock on a file in /tmp.
#include "../link.hpp"
#include <libusb.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <sys/file.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <pthread/qos.h>
#endif

namespace och::transport {
    namespace {
        constexpr uint16_t vendor_id = 0x1a86, product_id = 0x5537;

        constexpr uint8_t endpoint(Pipe p) {
            switch (p) {
            case Pipe::sample_out:
                return 0x01;
            case Pipe::sample_in:
                return 0x81;
            case Pipe::control_out:
                return 0x02;
            case Pipe::control_in:
                return 0x82;
            case Pipe::diag_out:
                return 0x03;
            case Pipe::diag_in:
                return 0x83;
            }
            return 0;
        }

        std::string error_text(int rc) {
            std::string text = libusb_error_name(rc);
#ifdef __linux__
            if (rc == LIBUSB_ERROR_NO_MEM) text += "; USB queue allocation failed: check usbcore.usbfs_memory_mb";
#endif
            return text;
        }

        // What to check when the device cannot be opened for lack of access.
        const char *access_hint() {
#ifdef __linux__
            return "; install packaging/linux/60-omnicore.rules and replug the device";
#else
            return "; close other programs that use the device";
#endif
        }

        // Called on the threads that carry samples: the stream worker and the libusb event thread.
        void prefer_sample_thread() {
#ifdef __APPLE__
            // The default QoS lets macOS move a busy thread to the efficiency cores.
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
        }

        // Keeps the computer from idle sleep while a stream runs. macOS only; best effort.
        class StayAwake {
        public:
            StayAwake() = default;

            ~StayAwake() { release(); }

            StayAwake(const StayAwake &) = delete;

            StayAwake &operator=(const StayAwake &) = delete;

            void hold() {
#ifdef __APPLE__
                if (!held)
                    held = IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                                       kIOPMAssertionLevelOn, CFSTR("OmniCoreSDR IQ streaming"),
                                                       &assertion) == kIOReturnSuccess;
#endif
            }

            void release() {
#ifdef __APPLE__
                if (held) IOPMAssertionRelease(assertion);
                held = false;
#endif
            }

        private:
#ifdef __APPLE__
            IOPMAssertionID assertion = 0;
            bool held = false;
#endif
        };

        const char *const cancelled_text = "USB transfer cancelled";

        // Why a finished transfer did not deliver its data.
        std::string status_text(libusb_transfer_status status) {
            switch (status) {
            case LIBUSB_TRANSFER_TIMED_OUT:
                return "USB transfer timed out";
            case LIBUSB_TRANSFER_CANCELLED:
                return cancelled_text;
            case LIBUSB_TRANSFER_NO_DEVICE:
                return error_text(LIBUSB_ERROR_NO_DEVICE);
            case LIBUSB_TRANSFER_STALL:
                return error_text(LIBUSB_ERROR_PIPE);
            case LIBUSB_TRANSFER_OVERFLOW:
                return error_text(LIBUSB_ERROR_OVERFLOW);
            default:
                return error_text(LIBUSB_ERROR_IO);
            }
        }

        // A timed-out IN request may contain complete control lines or stale
        // stream data. Preserve those bytes instead of silently dropping them.
        bool delivered(const libusb_transfer *t) {
            return t->status == LIBUSB_TRANSFER_COMPLETED ||
                   (t->status == LIBUSB_TRANSFER_TIMED_OUT && (t->endpoint & 0x80) && t->actual_length);
        }

        struct Context {
            libusb_context *p = nullptr;

            Context() {
                const int rc = libusb_init(&p);
                if (rc) throw std::runtime_error(libusb_error_name(rc));
            }

            ~Context() { libusb_exit(p); }

            Context(const Context &) = delete;

            Context &operator=(const Context &) = delete;
        };

        struct DeviceList {
            libusb_device **p = nullptr;
            ssize_t count;

            explicit DeviceList(libusb_context *c) : count(libusb_get_device_list(c, &p)) {
                if (count < 0) throw std::runtime_error(libusb_error_name(int(count)));
            }

            ~DeviceList() {
                if (p) libusb_free_device_list(p, 1);
            }

            DeviceList(const DeviceList &) = delete;

            DeviceList &operator=(const DeviceList &) = delete;
        };

        std::string device_path(libusb_device *d) {
            return std::to_string(libusb_get_bus_number(d)) + ":" + std::to_string(libusb_get_device_address(d));
        }

        // SampleQueue on libusb transfers allocated once and resubmitted. Completions arrive on
        // the link's event thread and are handed to the stream worker under this queue's lock.
        class LibusbQueue final : public SampleQueue {
            struct Slot : Transfer {
                LibusbQueue *queue = nullptr;
                libusb_transfer *native = nullptr;
                std::unique_ptr<uint8_t[]> heap;
                bool completed = false; // under queue->mutex, while the slot is in flight
            };

        public:
            LibusbQueue(libusb_device_handle *d, Pipe pipe, size_t transfer_bytes, unsigned depth)
                : device(d), ep(endpoint(pipe)), bytes(transfer_bytes), slots(depth), in_flight(depth) {
                try {
                    for (auto &s: slots) {
                        s.queue = this;
                        s.native = libusb_alloc_transfer(0);
                        if (!s.native) throw Error("cannot create USB event");
                        // Linux usbfs can map DMA-capable buffers directly, avoiding a kernel copy
                        // on every completion. Fall back to ordinary memory on unsupported kernels.
                        s.data = libusb_dev_mem_alloc(device, bytes);
                        if (!s.data) {
                            s.heap = std::make_unique<uint8_t[]>(bytes);
                            s.data = s.heap.get();
                        }
                        s.capacity = bytes;
                        memset(s.data, 0, bytes);
                    }
                } catch (...) {
                    release();
                    throw;
                }
            }

            ~LibusbQueue() override {
                stop();
                release();
            }

            size_t size() const override { return slots.size(); }

            Transfer &slot(size_t index) override { return slots[index]; }

            size_t max_transfer_bytes() override { return 1048576; }

            // libusb already delivers partial reads, with no auto flush, no short
            // packet padding, and all submitted transfers in endpoint order.
            void start() override {
                prefer_sample_thread();
                awake.hold();
            }

            void submit(Transfer &t) override {
                auto &s = static_cast<Slot &>(t);
                libusb_fill_bulk_transfer(s.native, device, ep, s.data, static_cast<int>(s.used), completion, &s, 0);
                std::lock_guard<std::mutex> lock(mutex);
                s.completed = false;
                const int rc = libusb_submit_transfer(s.native);
                if (rc) throw Error("USB streaming submission failed: " + error_text(rc));
                in_flight[(first + count) % in_flight.size()] = &s;
                ++count;
            }

            Wait wait(uint32_t timeout_ms, Transfer *&done, size_t &transferred) override {
                std::unique_lock<std::mutex> lock(mutex);
                Slot *s = in_flight[first];
                changed.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return woken || s->completed; });
                if (woken) return Wait::woken;
                if (!s->completed) return Wait::timeout;
                if (!delivered(s->native)) throw Error("USB transfer failed: " + status_text(s->native->status));
                pop();
                done = s;
                transferred = static_cast<size_t>(s->native->actual_length);
                return Wait::completed;
            }

            void wake() noexcept override {
                std::lock_guard<std::mutex> lock(mutex);
                woken = true;
                changed.notify_all();
            }

            uint64_t stop() override {
                std::unique_lock<std::mutex> lock(mutex);
                uint64_t unconfirmed = 0;
                for (size_t i = 0; i < count; ++i) {
                    Slot *s = in_flight[(first + i) % in_flight.size()];
                    unconfirmed += s->used;
                    if (!s->completed) libusb_cancel_transfer(s->native);
                }
                // Every cancelled transfer still gets its callback; the memory is reusable after it.
                changed.wait(lock, [&] {
                    for (size_t i = 0; i < count; ++i)
                        if (!in_flight[(first + i) % in_flight.size()]->completed) return false;
                    return true;
                });
                first = count = 0;
                awake.release();
                return unconfirmed;
            }

            void reset() override {
                std::lock_guard<std::mutex> lock(mutex);
                first = count = 0;
                for (auto &s: slots) s.used = 0;
                woken = false;
            }

        private:
            static void LIBUSB_CALL completion(libusb_transfer *t) {
                auto &s = *static_cast<Slot *>(t->user_data);
                std::lock_guard<std::mutex> lock(s.queue->mutex);
                s.completed = true;
                s.queue->changed.notify_all();
            }

            void pop() {
                first = (first + 1) % in_flight.size();
                --count;
            }

            void release() {
                for (auto &s: slots) {
                    if (s.native) libusb_free_transfer(s.native);
                    if (s.data && !s.heap) libusb_dev_mem_free(device, s.data, bytes);
                    s.native = nullptr;
                    s.data = nullptr;
                }
            }

            libusb_device_handle *device;
            const uint8_t ep;
            const size_t bytes;
            std::vector<Slot> slots;
            std::mutex mutex;
            std::condition_variable changed;
            // In flight, oldest first: a ring over in_flight, which holds one entry per slot.
            std::vector<Slot *> in_flight;
            size_t first = 0, count = 0;
            bool woken = false;
            StayAwake awake; // between start() and stop(), on the stream worker
        };

        class LibusbLink final : public Link {
        public:
            explicit LibusbLink(const DeviceId &id) {
                try {
                    open(id.path);
                } catch (...) {
                    close();
                    throw;
                }
            }

            ~LibusbLink() override { close(); }

            uint32_t speed_mbps() const override {
                const auto packet_size = pipes.at(endpoint(Pipe::sample_in));
                return packet_size == 1024 ? 5000 : packet_size == 512 ? 480 : 12;
            }

            bool has(Pipe p) const override { return pipes.count(endpoint(p)) != 0; }

            size_t vendor_in(uint8_t request, uint16_t value, uint16_t index, void *data, size_t size) override {
                return vendor(0xc0, request, value, index, data, size);
            }

            void vendor_out(uint8_t request, uint16_t value, uint16_t index, const void *data, size_t size) override {
                vendor(0x40, request, value, index, const_cast<void *>(data), size);
            }

            size_t transfer(Pipe p, uint8_t *data, size_t size, uint32_t timeout_ms) override {
                return bulk(endpoint(p), data, size, timeout_ms ? timeout_ms : 1);
            }

            // One persistent request. Repeated usbfs timeout/cancel cycles can stall xHCI service
            // to the high-rate IQ endpoints. shutdown_control() cancels it and blocks a racing
            // resubmission at close.
            size_t read_control(uint8_t *data, size_t size) override {
                return bulk(endpoint(Pipe::control_in), data, size, 0);
            }

            void shutdown_control() noexcept override {
                std::lock_guard<std::mutex> lock(mutex);
                control_stopping = true;
                for (auto *t: active)
                    if (t->endpoint == endpoint(Pipe::control_in)) libusb_cancel_transfer(t);
            }

            void reset_sample_pipes() override {
                // Synchronize endpoint halt/sequence state after firmware cleanup and
                // cancellation before submitting another stream through usbfs/xHCI.
                for (const Pipe p: {Pipe::sample_out, Pipe::sample_in}) {
                    const int rc = libusb_clear_halt(device, endpoint(p));
                    if (rc != 0) throw Error(std::string("EP1 reset failed: ") + libusb_error_name(rc));
                }
            }

            std::unique_ptr<SampleQueue> open_samples(Pipe p, size_t transfer_bytes, unsigned depth) override {
                return std::make_unique<LibusbQueue>(device, p, transfer_bytes, depth);
            }

        private:
            // One blocking transfer of bulk(): done is set by the callback under the link's mutex.
            struct Call {
                LibusbLink *link;
                bool done = false;
            };

            static void LIBUSB_CALL call_done(libusb_transfer *t) {
                auto &call = *static_cast<Call *>(t->user_data);
                std::lock_guard<std::mutex> lock(call.link->mutex);
                call.done = true;
                call.link->active.erase(t);
                call.link->finished.notify_all();
            }

            // One transfer, waited to completion. timeout 0 waits without limit.
            size_t bulk(uint8_t ep, uint8_t *data, size_t size, unsigned timeout) {
                std::unique_ptr<libusb_transfer, decltype(&libusb_free_transfer)> t(libusb_alloc_transfer(0),
                                                                                    libusb_free_transfer);
                if (!t) throw Error("submit: " + error_text(LIBUSB_ERROR_NO_MEM));
                Call call{this};
                libusb_fill_bulk_transfer(t.get(), device, ep, data, static_cast<int>(size), call_done, &call, timeout);
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    if (ep == endpoint(Pipe::control_in) && control_stopping)
                        throw Error(std::string("submit: ") + cancelled_text);
                    active.insert(t.get());
                    const int rc = libusb_submit_transfer(t.get());
                    if (rc) {
                        active.erase(t.get());
                        throw Error("submit: " + error_text(rc));
                    }
                    finished.wait(lock, [&] { return call.done; });
                }
                if (delivered(t.get())) return static_cast<size_t>(t->actual_length);
                if (t->status == LIBUSB_TRANSFER_TIMED_OUT) return 0;
                throw Error("completion: " + status_text(t->status));
            }

            size_t vendor(uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *data, size_t size) {
                const int rc =
                        libusb_control_transfer(device, type, request, value, index, static_cast<unsigned char *>(data),
                                                static_cast<uint16_t>(size), 2000);
                if (rc < 0) throw Error(error_text(rc));
                return static_cast<size_t>(rc);
            }

            void open(const std::string &path) {
                int rc = libusb_init(&context);
                if (rc) throw std::runtime_error(libusb_error_name(rc));
                {
                    DeviceList list(context);
                    for (ssize_t i = 0; i < list.count; ++i)
                        if (device_path(list.p[i]) == path) {
                            rc = libusb_open(list.p[i], &device);
                            if (rc)
                                throw std::runtime_error(std::string("Cannot open OmniCore USB: ") +
                                                         libusb_error_name(rc) +
                                                         (rc == LIBUSB_ERROR_ACCESS ? access_hint() : ""));
                            break;
                        }
                }
                if (!device) throw std::runtime_error("USB device disappeared; enumerate again");
                libusb_config_descriptor *raw = nullptr;
                rc = libusb_get_active_config_descriptor(libusb_get_device(device), &raw);
#ifdef __APPLE__
                if (rc == LIBUSB_ERROR_NOT_FOUND) {
                    // macOS leaves a device with no kernel driver unconfigured; select its first configuration.
                    rc = libusb_get_config_descriptor(libusb_get_device(device), 0, &raw);
                    if (rc)
                        throw std::runtime_error(std::string("Cannot read OmniCore USB configuration: ") +
                                                 libusb_error_name(rc));
                    const int value = raw->bConfigurationValue;
                    libusb_free_config_descriptor(raw);
                    raw = nullptr;
                    rc = libusb_set_configuration(device, value);
                    if (rc)
                        throw std::runtime_error(std::string("Cannot configure OmniCore USB: ") +
                                                 libusb_error_name(rc));
                    rc = libusb_get_active_config_descriptor(libusb_get_device(device), &raw);
                }
#endif
                if (rc) throw std::runtime_error(libusb_error_name(rc));
                std::unique_ptr<libusb_config_descriptor, decltype(&libusb_free_config_descriptor)> config(
                        raw, libusb_free_config_descriptor);
                int selected = -1;
                for (unsigned i = 0; i < raw->bNumInterfaces && selected < 0; ++i) {
                    const auto &interface = raw->interface[i];
                    for (int j = 0; j < interface.num_altsetting; ++j) {
                        const auto &alt = interface.altsetting[j];
                        if (alt.bAlternateSetting != 0) continue;
                        std::map<uint8_t, uint16_t> candidate; // bulk endpoint address -> max packet size
                        for (unsigned k = 0; k < alt.bNumEndpoints; ++k) {
                            const auto &e = alt.endpoint[k];
                            if ((e.bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK)
                                candidate[e.bEndpointAddress] = e.wMaxPacketSize;
                        }
                        if (candidate.count(0x81) && candidate.count(0x01) && candidate.count(0x82) &&
                            candidate.count(0x02)) {
                            selected = alt.bInterfaceNumber;
                            pipes = std::move(candidate);
                            break;
                        }
                    }
                }
                if (selected < 0) throw std::runtime_error("OmniCore bulk endpoints 01/81/02/82 not found");
                rc = libusb_claim_interface(device, selected);
                if (rc)
                    throw std::runtime_error(std::string("Cannot claim OmniCore interface: ") + libusb_error_name(rc));
                interface_number = selected;
                events = std::thread([this] {
                    prefer_sample_thread();
                    while (!stopping) {
                        timeval timeout{0, 50000};
                        libusb_handle_events_timeout_completed(context, &timeout, nullptr);
                    }
                });
            }

            void close() {
                if (device) {
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        for (auto *t: active) libusb_cancel_transfer(t);
                        finished.wait(lock, [&] { return active.empty(); });
                    }
                    stopping = true;
                    libusb_interrupt_event_handler(context);
                    if (events.joinable()) events.join();
                    if (interface_number >= 0) libusb_release_interface(device, interface_number);
                    libusb_close(device);
                    device = nullptr;
                }
                if (context) libusb_exit(context);
                context = nullptr;
            }

            libusb_context *context = nullptr;
            libusb_device_handle *device = nullptr;
            int interface_number = -1;
            std::map<uint8_t, uint16_t> pipes;
            std::atomic<bool> stopping{false};
            std::thread events;
            // Guards active and control_stopping; finished is signalled when a bulk() call ends.
            std::mutex mutex;
            std::condition_variable finished;
            std::set<libusb_transfer *> active;
            bool control_stopping = false;
        };
    } // namespace

    std::vector<DeviceId> enumerate() {
        Context context;
        DeviceList list(context.p);
        std::vector<DeviceId> found;
        for (ssize_t i = 0; i < list.count; ++i) {
            auto *d = list.p[i];
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(d, &desc) || desc.idVendor != vendor_id || desc.idProduct != product_id)
                continue;
            std::string serial;
            libusb_device_handle *h = nullptr;
            if (!libusb_open(d, &h)) {
                unsigned char text[256]{};
                const int n = desc.iSerialNumber
                                      ? libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, text, sizeof text)
                                      : 0;
                if (n > 0) serial.assign(reinterpret_cast<char *>(text), static_cast<size_t>(n));
                libusb_close(h);
            }
            if (serial.empty()) {
                uint8_t ports[8]{};
                const int n = libusb_get_port_numbers(d, ports, sizeof ports);
                serial = "usb-" + std::to_string(libusb_get_bus_number(d));
                for (int j = 0; j < n; ++j) serial += "-" + std::to_string(ports[j]);
            }
            found.push_back({device_path(d), serial});
        }
        return found;
    }

    std::unique_ptr<Link> open(const DeviceId &id) { return std::make_unique<LibusbLink>(id); }

    const char *driver_name() { return "libusb-1.0"; }

    const char *not_found_message() { return "OmniCoreSDR USB device not found (VID 1a86, PID 5537)"; }

    // flock also releases ownership after a crash. Do not unlink the lock inode:
    // waiters and new openers must always contend on the same file.
    DeviceLock::Result DeviceLock::acquire(const std::string &serial) {
        // Same file name as earlier releases, so old and new processes exclude each other.
        std::string path = "/tmp/";
        for (const char c: "Local\\OmniCoreHost_" + serial)
            path += (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? c : '_';
        path += ".lock";
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
        if (fd < 0) return Result::unavailable;
        const bool locked = flock(fd, LOCK_EX | LOCK_NB) == 0;
        if (!locked && errno != EWOULDBLOCK) {
            ::close(fd);
            return Result::unavailable;
        }
        held = std::shared_ptr<void>(nullptr, [fd](void *) { ::close(fd); });
        return locked ? Result::acquired : Result::busy;
    }
} // namespace och::transport
