#include "internal.hpp"
#include <charconv>
#include <cstdio>
#include <iomanip>
#include <sstream>
using namespace och::core;
using namespace och::transport;

namespace {
    ULONG transfer(Usb &u, UCHAR ep, uint8_t *data, ULONG size, ULONG timeout) {
        u.timeout(ep, timeout);
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(event.h != nullptr, OCH_IO, "USB control event allocation failed");
        OVERLAPPED ov{};
        ov.hEvent = event.h;
        ULONG n = 0;
        BOOL ok = (ep & 128) ? u.api.read(u.h, ep, data, size, &n, &ov) : u.api.write(u.h, ep, data, size, &n, &ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING)
            throw Failure(OCH_IO, "USB control submit: " + utf8(syserr(GetLastError())));
        if (!ok && !u.api.result(u.h, &ov, &n, TRUE)) {
            auto err = GetLastError();
            if (err == ERROR_SEM_TIMEOUT) return 0;
            throw Failure(OCH_IO, "USB control completion: " + utf8(syserr(err)));
        }
        return n;
    }
    std::string value(const std::string &text, const std::string &key) {
        auto i = text.find(key + "=");
        if (i == std::string::npos) return {};
        i += key.size() + 1;
        auto j = text.find_first_of(" \r\n", i);
        return text.substr(i, j - i);
    }
}
std::string och::core::serial_of(const DeviceInfo &d) {
    auto s = d.instance;
    auto i = s.find_last_of(L'\\');
    return utf8(i == std::wstring::npos ? s : s.substr(i + 1));
}
void och::core::fill_info(och_device_info &info, const DeviceInfo &d) {
    info = {};
    info.struct_size = sizeof(info);
    info.abi_version = OCH_ABI_VERSION;
    copy_text(info.serial, sizeof info.serial, serial_of(d));
    copy_text(info.name, sizeof info.name, "OmniCoreSDR");
#ifdef _WIN32
    copy_text(info.driver, sizeof info.driver, "libusbK");
#else
    copy_text(info.driver, sizeof info.driver, "libusb-1.0");
#endif
    info.rx_channels = info.tx_channels = 2;
    info.max_sample_rate = 61440000;
}
Device::~Device() {
    control_stop = true;
#ifndef _WIN32
    usb.stop_control();
#endif
    if (control_thread.joinable()) control_thread.join();
}
void Device::open(const DeviceInfo &d) {
    fill_info(info, d);
    std::wstring name = L"Local\\OmniCoreHost_" + wide(info.serial);
    owner.h = CreateMutexW(nullptr, FALSE, name.c_str());
    auto existed = GetLastError() == ERROR_ALREADY_EXISTS;
    require(owner.h != nullptr, OCH_IO, "device mutex unavailable");
    require(!existed, OCH_BUSY, "device already owned by another OmniCore Host process");
    usb.open(d.path);
    require(usb.has(0x82) && usb.has(0x02), OCH_PROTOCOL, "control endpoints 02/82 missing");
    // The bridge bootloader keeps the USB identity but has no SDK behind it.
    firmware_probe(usb, firmware, info.serial);
    require(firmware.mode != OCH_FW_MODE_BOOTLOADER, OCH_UNSUPPORTED,
            "OmniCoreSDR is in bridge bootloader mode; run och firmware update FILE or och firmware boot");
    const auto packet_size = usb.pipes.at(0x81).MaximumPacketSize;
    info.usb_speed_mbps = packet_size == 1024 ? 5000 : packet_size == 512 ? 480 : 12;
    require(packet_size == 1024 || packet_size == 512, OCH_UNSUPPORTED,
            "OmniCore Host requires USB 2.0 High-Speed (480 Mbit/s) or USB 3.0 SuperSpeed; Full-Speed is unsupported");
    usb.policy(0x82, RAW_IO, FALSE);
    usb.policy(0x82, ALLOW_PARTIAL_READS, TRUE);
    usb.policy(0x82, AUTO_FLUSH, FALSE);
    control_thread = std::thread([this] { control_loop(); });
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
    std::lock_guard<std::mutex> lock(events_mutex);
    e.sequence = ++event_seq;
    if (events.size() == 256) {
        events.pop_front();
        ++event_drops;
    }
    events.push_back(e);
    events_cv.notify_one();
}
void Device::line(const std::string &s) {
    if (s.rfind("EVT ", 0) == 0) {
        if (och::core::timed_event(*this, s)) return;
        if (s.find("type=rx_overflow") != std::string::npos && s.find("action=waiting_rx_resync") != std::string::npos)
            ++stats.overflows;
        if (s.find("type=tx_underflow") != std::string::npos && s.find("action=clearing_continue") != std::string::npos)
            ++stats.underflows;
        emit(OCH_EVENT_DEVICE, s);
        return;
    }
    std::lock_guard<std::mutex> lock(reply_mutex);
    if (reply_done) return;
    if (s.rfind("RSP id=", 0) == 0) {
        auto p = s.data() + 7;
        uint32_t id = 0;
        auto parsed = std::from_chars(p, s.data() + s.size(), id);
        if (parsed.ec != std::errc() || !pending_id || id != pending_id) return;
        std::string tail(parsed.ptr, s.data() + s.size());
        if (tail == " begin") {
            reply_started = true;
            return;
        }
        if (tail.rfind(" end result=", 0) == 0) {
            if (!reply_started) {
                control_failed = true;
                control_error = "command end arrived without begin; reply integrity lost";
                reply_cv.notify_all();
                return;
            }
            int code = 0;
            auto rc = std::from_chars(tail.data() + 12, tail.data() + tail.size(), code);
            if (rc.ec != std::errc() || rc.ptr != tail.data() + tail.size()) {
                control_failed = true;
                control_error = "malformed command completion";
            }
            reply_result = code;
            reply_done = true;
            reply_cv.notify_all();
            return;
        }
    }
    if (pending_id && reply_started && !reply_done) {
        if (reply.size() + s.size() + 1 > 32768) {
            control_failed = true;
            control_error = "command reply exceeded 32 KiB";
            reply_cv.notify_all();
            return;
        }
        reply += s;
        reply += '\n';
    }
}
void Device::control_loop() {
    std::string partial;
    partial.reserve(4096);
    try {
        std::array<uint8_t, 4096> bytes{};
        while (!control_stop) {
            // Linux uses one persistent request. Repeated usbfs timeout/cancel
            // cycles can stall xHCI service to the high-rate IQ endpoints.
            // stop_control cancels it and blocks a racing resubmission at close.
#ifdef _WIN32
            constexpr ULONG read_timeout = 50;
#else
            constexpr ULONG read_timeout = 0;
#endif
            auto n = transfer(usb, 0x82, bytes.data(), static_cast<ULONG>(bytes.size()), read_timeout);
            for (ULONG i = 0; i < n; ++i) {
                char c = static_cast<char>(bytes[i]);
                if (c == '\r') continue;
                if (c == '\n') {
                    if (!partial.empty()) line(partial);
                    partial.clear();
                } else {
                    if (partial.size() >= 32768) throw Failure(OCH_PROTOCOL, "unterminated control line");
                    partial += c;
                }
            }
        }
    } catch (const std::exception &e) {
        if (control_stop) return;
        {
            std::lock_guard<std::mutex> lock(reply_mutex);
            control_failed = true;
            control_error = e.what();
        }
        reply_cv.notify_all();
        emit(OCH_EVENT_TRANSPORT, std::string("control disconnected: ") + e.what());
    }
}
std::string Device::command(const std::string &cmd, uint32_t timeout) {
    auto first = cmd.find_first_not_of(" \t");
    require(first != std::string::npos && cmd[first] != '@' &&
                    std::none_of(cmd.begin(), cmd.end(),
                                 [](unsigned char c) { return (c < 32 && c != '\t') || c >= 127; }),
            OCH_INVALID, "command must be one ASCII SDK command");
    std::lock_guard<std::mutex> serial(command_mutex);
    uint32_t id = ++next_id;
    if (!id) id = ++next_id;
    std::string framed = "@" + std::to_string(id) + " " + cmd + "\n";
    require(framed.size() <= 128, OCH_INVALID, "command exceeds SDK 127-character line limit");
    {
        std::lock_guard<std::mutex> lock(reply_mutex);
        require(!control_failed, OCH_IO, control_error);
        pending_id = id;
        reply.clear();
        reply_done = reply_started = false;
        reply_result = 0;
    }
    bool write_complete = false;
    try {
        auto n = transfer(usb, 0x02, reinterpret_cast<uint8_t *>(framed.data()), static_cast<ULONG>(framed.size()),
                          2000);
        require(n == framed.size(), OCH_IO, "short USB command write");
        write_complete = true;
        std::unique_lock<std::mutex> lock(reply_mutex);
        bool done = reply_cv.wait_for(lock, std::chrono::milliseconds(timeout),
                                      [&] { return reply_done || control_failed; });
        if (!done) {
            control_failed = true;
            control_error = "SDK command timed out: " + cmd +
                            "; close/reopen device after firmware responds (envelope v1 required)";
            throw Failure(OCH_TIMEOUT, control_error);
        }
        require(!control_failed, OCH_IO, control_error);
        auto text = reply;
        auto rc = reply_result;
        pending_id = 0;
        require(!rc && text.find("ERR ") == std::string::npos && text.find("Error:") == std::string::npos &&
                        text.find("Invalid ") == std::string::npos,
                OCH_PROTOCOL, "SDK rejected " + cmd + ": " + text);
        return text;
    } catch (...) {
        std::lock_guard<std::mutex> lock(reply_mutex);
        pending_id = 0;
        if (!write_complete && !control_failed) {
            control_failed = true;
            control_error = "USB command write incomplete; close/reopen before sending another command";
        }
        throw;
    }
}
void Device::reset_transport() {
    require(!rx_on && !tx_on, OCH_BUSY, "transport reset requires both directions stopped");
    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = 0xc0;
    setup.Request = 0xd0;
    setup.Length = 256;
    uint32_t words[64]{};
    ULONG done = 0;
    require(usb.api.control(usb.h, setup, reinterpret_cast<UCHAR *>(words), sizeof words, &done, nullptr) != FALSE,
            OCH_IO, "CH32 diagnostics unavailable");
    require(done >= 8 && words[0] == 1396983601U && words[1] == 4, OCH_UNSUPPORTED,
            "CH32 transport reset protocol v4 required; firmware was not changed");
    setup.Value = 2;
    require(usb.api.control(usb.h, setup, reinterpret_cast<UCHAR *>(words), sizeof words, &done, nullptr) != FALSE,
            OCH_IO, "CH32 stream cleanup failed");
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
        auto n = transfer(usb, 0x81, junk.data(), 16384, 10);
        if (!n) {
            transport_dirty = false;
            return;
        }
        drained += n;
    }
    throw Failure(OCH_IO, "RX stale data did not drain (" + std::to_string(drained) + " bytes)");
}
