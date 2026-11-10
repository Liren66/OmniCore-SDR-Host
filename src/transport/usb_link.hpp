// Link implemented on the existing Usb class, shared by both platforms. Temporary: core moves
// onto Link one operation at a time, then each platform implements Link directly
// (TRANSPORT_PLAN.md, 2.9 / 2.10) and this adapter is removed.
#pragma once
#include "link.hpp"
#include "transport.hpp"

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

    // Opens the Usb a Device or firmware session still holds directly.
    inline void open_usb(Usb &usb, const DeviceId &id) { usb.open(wide(id.path)); }

    // Does not own the Usb: the Device still holds it for the call sites not yet moved.
    class UsbLink final : public Link {
    public:
        explicit UsbLink(Usb &u) : usb(u) {}

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

        Usb &usb;
        bool control_in_ready = false, diag_in_ready = false;
    };
} // namespace och::transport
