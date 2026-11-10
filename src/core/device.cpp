#include "internal.hpp"
#include "../transport/usb_link.hpp"
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <sstream>
using namespace och::core;
using namespace och::transport;

namespace {
    std::string value(const std::string &text, const std::string &key) {
        auto i = text.find(key + "=");
        if (i == std::string::npos) return {};
        i += key.size() + 1;
        auto j = text.find_first_of(" \r\n", i);
        return text.substr(i, j - i);
    }
}
void och::core::fill_info(och_device_info &info, const DeviceId &d) {
    info = {};
    info.struct_size = sizeof(info);
    info.abi_version = OCH_ABI_VERSION;
    copy_text(info.serial, sizeof info.serial, d.serial);
    copy_text(info.name, sizeof info.name, "OmniCoreSDR");
    copy_text(info.driver, sizeof info.driver, driver_name());
    info.rx_channels = info.tx_channels = 2;
    info.max_sample_rate = 61440000;
}
Device::~Device() { control.stop(); }
void Device::open(const DeviceId &d) {
    fill_info(info, d);
    const auto lock = owner.acquire(info.serial);
    require(lock != DeviceLock::Result::unavailable, OCH_IO, "device mutex unavailable");
    require(lock != DeviceLock::Result::busy, OCH_BUSY, "device already owned by another OmniCore Host process");
    open_usb(usb, d);
    attach(std::make_unique<UsbLink>(usb));
}
void Device::attach(std::unique_ptr<Link> opened) {
    link = std::move(opened);
    require(link->has(Pipe::control_in) && link->has(Pipe::control_out), OCH_PROTOCOL,
            "control endpoints 02/82 missing");
    // The bridge bootloader keeps the USB identity but has no SDK behind it.
    firmware_probe(*link, firmware, info.serial);
    require(firmware.mode != OCH_FW_MODE_BOOTLOADER, OCH_UNSUPPORTED,
            "OmniCoreSDR is in bridge bootloader mode; run och firmware update FILE or och firmware boot");
    info.usb_speed_mbps = link->speed_mbps();
    require(info.usb_speed_mbps == 5000 || info.usb_speed_mbps == 480, OCH_UNSUPPORTED,
            "OmniCore Host requires USB 2.0 High-Speed (480 Mbit/s) or USB 3.0 SuperSpeed; Full-Speed is unsupported");
    control.start(
            *link, [this](const std::string &line) { event_line(line); },
            [this](const std::string &why) { emit(OCH_EVENT_TRANSPORT, "control disconnected: " + why); });
    auto p = command("host_protocol?", 3000);
    require(p.find("host_protocol=1") != std::string::npos, OCH_UNSUPPORTED,
            "SDK command envelope v1 required; see firmware/README");
    auto s = command("system_info?");
    require(s.find("transport=framed16k") != std::string::npos, OCH_UNSUPPORTED,
            "framed USB transport required: load FPGA CH32H417_IP 1.3 and SDK_CMD_1.1 (OCH 0.6 has no legacy preamble mode)");
    copy_text(info.firmware, sizeof info.firmware, value(s, "firmware"));
    capabilities = s;
    info.hardware_timestamps = supports_capability("hw_time") ? 1 : 0;
    command("usb_stop!");
    rx_on = tx_on = false;
    och_default_config(&config);
    reset_transport();
}
void Device::emit(uint32_t type, const std::string &message, uint64_t ep, uint64_t hw_time, uint32_t code,
                  uint32_t block, uint32_t count) {
    och_event e{};
    e.struct_size = sizeof e;
    e.type = type;
    e.host_monotonic_ns = now_ns();
    e.epoch = ep;
    e.hw_time = hw_time;
    e.code = code;
    e.block = block;
    e.count = count ? count : 1;
    copy_text(e.message, sizeof e.message, message);
    events.push(e);
}
void Device::check_stream_slot(const och_stream_args &a) {
    require(configured, OCH_INVALID, "configure before creating streams");
    require(a.format != OCH_SC8 || iq_bits(config) == 8, OCH_INVALID,
            "sc8 application format requires 8-bit USB IQ mode");
    require((a.direction == OCH_RX ? rx : tx) == nullptr, OCH_BUSY, "only one stream per direction");
    require((a.direction == OCH_RX ? config.rx_mask : config.tx_mask) != 0, OCH_INVALID,
            "stream direction has no enabled channels");
}
std::string Device::detach_stream(Stream &s) {
    require(!s.reader.lease, OCH_BUSY, "release RX lease before destroying stream");
    std::string issue;
    try {
        s.stop(0);
    } catch (const std::exception &e) {
        issue = e.what();
    }
    if (s.args.direction == OCH_RX)
        rx = nullptr;
    else
        tx = nullptr;
    return issue;
}
void EventQueue::push(och_event e) {
    std::lock_guard<std::mutex> lock(mutex);
    e.sequence = ++seq;
    if (queue.size() == capacity) {
        queue.pop_front();
        ++dropped;
    }
    queue.push_back(e);
    cv.notify_one();
}
bool EventQueue::pop(och_event &out, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return !queue.empty(); })) return false;
    out = queue.front();
    queue.pop_front();
    return true;
}
void Device::event_line(const std::string &s) {
    if (och::core::timed_event(*this, s)) return;
    if (s.find("type=rx_overflow") != std::string::npos && s.find("action=waiting_rx_resync") != std::string::npos)
        ++stats.overflows;
    if (s.find("type=tx_underflow") != std::string::npos && s.find("action=clearing_continue") != std::string::npos)
        ++stats.underflows;
    emit(OCH_EVENT_DEVICE, s);
}
std::string Device::command(const std::string &cmd, uint32_t timeout) { return control.command(cmd, timeout); }
void Device::reset_transport() {
    require(!rx_on && !tx_on, OCH_BUSY, "transport reset requires both directions stopped");
    // CH32 bridge diagnostics request: value 0 reads the protocol id, value 2 clears its stream buffers.
    constexpr uint8_t bridge_diagnostics = 0xd0;
    uint32_t words[64]{};
    size_t done = 0;
    try {
        done = link->vendor_in(bridge_diagnostics, 0, 0, words, sizeof words);
    } catch (const Error &) {
        throw Failure(OCH_IO, "CH32 diagnostics unavailable");
    }
    require(done >= 8 && words[0] == 1396983601U && words[1] == 4, OCH_UNSUPPORTED,
            "CH32 transport reset protocol v4 required; firmware was not changed");
    try {
        link->vendor_in(bridge_diagnostics, 2, 0, words, sizeof words);
    } catch (const Error &) {
        throw Failure(OCH_IO, "CH32 stream cleanup failed");
    }
#ifndef _WIN32
    // Synchronize endpoint halt/sequence state after firmware cleanup and
    // cancellation before submitting another stream through usbfs/xHCI.
    for (unsigned char ep: {0x01, 0x81}) {
        const int rc = libusb_clear_halt(usb.device, ep);
        require(rc == 0, OCH_IO, std::string("EP1 reset failed: ") + libusb_error_name(rc));
    }
#endif
    usb.policy(0x81, RAW_IO, FALSE);
    usb.policy(0x81, ALLOW_PARTIAL_READS, TRUE);
    std::vector<uint8_t> junk(16384);
    size_t drained = 0;
    for (unsigned i = 0; i < 128; ++i) {
        auto n = bounded_transfer(*link, Pipe::sample_in, junk.data(), junk.size(), 10, "USB control");
        if (!n) {
            transport_dirty = false;
            return;
        }
        drained += n;
    }
    throw Failure(OCH_IO, "RX stale data did not drain (" + std::to_string(drained) + " bytes)");
}
