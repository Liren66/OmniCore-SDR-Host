// Link implemented on the existing Usb class, shared by both platforms; included only by
// usb_link.cpp. Temporary: each platform will implement Link directly (TRANSPORT_PLAN.md,
// 2.9 / 2.10) and this adapter is removed.
#pragma once
#include "link.hpp"
#include "transport.hpp"
#include <vector>

namespace och::transport {
    constexpr UCHAR endpoint(Pipe p) {
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

    // SampleQueue on overlapped transfers: one event per transfer plus the wake event.
    class UsbSampleQueue final : public SampleQueue {
        struct Slot : Transfer {
            OVERLAPPED ov{};
            Handle event;
            TransferBuffer bytes;
            bool pending = false;

            Slot(Usb &usb, size_t n) : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), bytes(usb, n) {
                if (!event.h) throw Error("cannot create USB event");
                ov.hEvent = event.h;
                data = bytes.get();
                capacity = n;
                memset(data, 0, n);
            }
        };

    public:
        UsbSampleQueue(Usb &u, Pipe pipe, size_t transfer_bytes, unsigned depth)
            : usb(u), ep(endpoint(pipe)), wake_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), active(depth) {
            if (!wake_event.h) throw Error("stream stop event allocation failed");
            slots.reserve(depth);
            for (unsigned i = 0; i < depth; ++i) slots.push_back(std::make_unique<Slot>(usb, transfer_bytes));
        }

        size_t size() const override { return slots.size(); }

        Transfer &slot(size_t index) override { return *slots[index]; }

        size_t max_transfer_bytes() override { return usb.maximum(ep); }

        void start() override {
#ifdef _WIN32
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
            usb.timeout(ep, 0);
            usb.policy(ep, RAW_IO, FALSE);
            if (ep & 0x80) {
                usb.policy(ep, IGNORE_SHORT_PACKETS, FALSE);
                usb.policy(ep, ALLOW_PARTIAL_READS, TRUE);
                usb.policy(ep, AUTO_FLUSH, FALSE);
            } else {
                usb.policy(ep, SHORT_PACKET_TERMINATE, FALSE);
                usb.parallelLimit(ep, static_cast<ULONG>(slots.size()));
            }
            usb.policy(ep, RAW_IO, TRUE);
        }

        void submit(Transfer &t) override {
            auto &s = static_cast<Slot &>(t);
            ResetEvent(s.ov.hEvent);
            const BOOL ok = (ep & 0x80) ? usb.api.read(usb.h, ep, s.data, static_cast<ULONG>(s.used), nullptr, &s.ov)
                                        : usb.api.write(usb.h, ep, s.data, static_cast<ULONG>(s.used), nullptr, &s.ov);
            if (!ok && GetLastError() != ERROR_IO_PENDING)
                throw Error("USB streaming submission failed: " + utf8(syserr(GetLastError())));
            if (ok) SetEvent(s.ov.hEvent);
            s.pending = true;
            active[(first + count) % active.size()] = &s;
            ++count;
        }

        Wait wait(uint32_t timeout_ms, Transfer *&done, size_t &bytes) override {
            Slot *s = active[first];
            HANDLE events[2] = {wake_event.h, s->ov.hEvent};
            const DWORD w = WaitForMultipleObjects(2, events, FALSE, timeout_ms);
            if (w == WAIT_OBJECT_0) return Wait::woken;
            if (w == WAIT_TIMEOUT) return Wait::timeout;
            if (w != WAIT_OBJECT_0 + 1) throw Error("USB event wait failed");
            ULONG n = 0;
            if (!usb.api.result(usb.h, &s->ov, &n, FALSE))
                throw Error("USB transfer failed: " + utf8(syserr(GetLastError())));
            pop();
            s->pending = false;
            done = s;
            bytes = n;
            return Wait::completed;
        }

        void wake() noexcept override { SetEvent(wake_event.h); }

        uint64_t stop() override {
            uint64_t unconfirmed = 0;
            if (count) {
                usb.api.abort(usb.h, ep);
                while (count) {
                    Slot *s = active[first];
                    pop();
                    unconfirmed += s->used;
#ifdef _WIN32
                    CancelIoEx(usb.file, &s->ov);
#endif
                    ULONG n = 0;
                    usb.api.result(usb.h, &s->ov, &n, TRUE);
                    s->pending = false;
                }
            }
            UCHAR off = FALSE;
            usb.api.setPolicy(usb.h, ep, RAW_IO, sizeof off, &off);
            return unconfirmed;
        }

        void reset() override {
            first = count = 0;
            for (auto &s: slots) {
                s->used = 0;
                s->pending = false;
                s->ov = {};
                s->ov.hEvent = s->event.h;
                ResetEvent(s->event.h);
            }
            ResetEvent(wake_event.h);
        }

    private:
        void pop() {
            first = (first + 1) % active.size();
            --count;
        }

        Usb &usb;
        UCHAR ep;
        Handle wake_event;
        std::vector<std::unique_ptr<Slot>> slots;
        // In flight, oldest first: a ring over active, which holds one entry per slot.
        std::vector<Slot *> active;
        size_t first = 0, count = 0;
    };

    class UsbLink final : public Link {
    public:
        explicit UsbLink(const DeviceId &id) { usb.open(wide(id.path)); }

        uint32_t speed_mbps() const override {
            const auto packet_size = usb.pipes.at(endpoint(Pipe::sample_in)).MaximumPacketSize;
            return packet_size == 1024 ? 5000 : packet_size == 512 ? 480 : 12;
        }

        bool has(Pipe p) const override { return usb.has(endpoint(p)); }

        size_t vendor_in(uint8_t request, uint16_t value, uint16_t index, void *data, size_t size) override {
            return vendor(0xc0, request, value, index, data, size);
        }

        void vendor_out(uint8_t request, uint16_t value, uint16_t index, const void *data, size_t size) override {
            vendor(0x40, request, value, index, const_cast<void *>(data), size);
        }

        size_t transfer(Pipe p, uint8_t *data, size_t size, uint32_t timeout_ms) override {
            prepare(p);
            return bulk(endpoint(p), data, size, timeout_ms ? timeout_ms : 1);
        }

        size_t read_control(uint8_t *data, size_t size) override {
            // Linux uses one persistent request. Repeated usbfs timeout/cancel
            // cycles can stall xHCI service to the high-rate IQ endpoints.
            // stop_control cancels it and blocks a racing resubmission at close.
#ifdef _WIN32
            constexpr ULONG read_timeout = 50;
#else
            constexpr ULONG read_timeout = 0;
#endif
            prepare(Pipe::control_in);
            return bulk(endpoint(Pipe::control_in), data, size, read_timeout);
        }

        void shutdown_control() noexcept override {
#ifndef _WIN32
            usb.stop_control();
#endif
        }

        void reset_sample_pipes() override {
#ifndef _WIN32
            // Synchronize endpoint halt/sequence state after firmware cleanup and
            // cancellation before submitting another stream through usbfs/xHCI.
            for (const Pipe p: {Pipe::sample_out, Pipe::sample_in}) {
                const int rc = libusb_clear_halt(usb.device, endpoint(p));
                if (rc != 0) throw Error(std::string("EP1 reset failed: ") + libusb_error_name(rc));
            }
#endif
            usb.policy(endpoint(Pipe::sample_in), RAW_IO, FALSE);
            usb.policy(endpoint(Pipe::sample_in), ALLOW_PARTIAL_READS, TRUE);
        }

        std::unique_ptr<SampleQueue> open_samples(Pipe p, size_t transfer_bytes, unsigned depth) override {
            return std::make_unique<UsbSampleQueue>(usb, p, transfer_bytes, depth);
        }

    private:
        // Line-oriented IN pipes are read without RAW_IO, accepting short reads. Set on first
        // use; each flag belongs to the one context that reads its pipe (control thread for
        // control_in, the device's state_mutex for diag_in).
        void prepare(Pipe p) {
            if (p != Pipe::control_in && p != Pipe::diag_in) return;
            bool &done = p == Pipe::control_in ? control_in_ready : diag_in_ready;
            if (done) return;
            usb.policy(endpoint(p), RAW_IO, FALSE);
            usb.policy(endpoint(p), ALLOW_PARTIAL_READS, TRUE);
            usb.policy(endpoint(p), AUTO_FLUSH, FALSE);
            done = true;
        }

        // One overlapped transfer, waited to completion. timeout 0 waits without limit.
        size_t bulk(UCHAR ep, uint8_t *data, size_t size, ULONG timeout) {
            usb.timeout(ep, timeout);
            Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.h) throw Error("event allocation failed");
            OVERLAPPED ov{};
            ov.hEvent = event.h;
            ULONG count = 0;
            const BOOL ok = (ep & 0x80) ? usb.api.read(usb.h, ep, data, static_cast<ULONG>(size), &count, &ov)
                                        : usb.api.write(usb.h, ep, data, static_cast<ULONG>(size), &count, &ov);
            const auto submit_error = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && submit_error == ERROR_SEM_TIMEOUT) return 0;
            if (!ok && submit_error != ERROR_IO_PENDING) throw Error("submit: " + utf8(syserr(submit_error)));
            if (!ok && !usb.api.result(usb.h, &ov, &count, TRUE)) {
                const auto code = GetLastError();
                if (code == ERROR_SEM_TIMEOUT) return 0;
                throw Error("completion: " + utf8(syserr(code)));
            }
            return count;
        }

        size_t vendor(UCHAR type, UCHAR request, uint16_t value, uint16_t index, void *data, size_t size) {
            WINUSB_SETUP_PACKET setup{};
            setup.RequestType = type;
            setup.Request = request;
            setup.Value = value;
            setup.Index = index;
            setup.Length = static_cast<uint16_t>(size);
            ULONG done = 0;
            if (!usb.api.control(usb.h, setup, static_cast<PUCHAR>(data), static_cast<ULONG>(size), &done, nullptr))
                throw Error(utf8(syserr(GetLastError())));
            return done;
        }

        Usb usb;
        bool control_in_ready = false, diag_in_ready = false;
    };
} // namespace och::transport
