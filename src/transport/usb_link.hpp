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

    private:
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
    };
} // namespace och::transport
