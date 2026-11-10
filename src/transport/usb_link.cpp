// Enumeration and the cross-process device lock on the existing per-platform transport headers.
#include "usb_link.hpp"

namespace och::transport {
    std::vector<DeviceId> enumerate() {
        std::vector<DeviceId> out;
        for (const auto &d: scanDevices(0x1a86, 0x5537)) {
            // Windows: the serial is the last component of the device instance id.
            const auto i = d.instance.find_last_of(L'\\');
            out.push_back({utf8(d.path), utf8(i == std::wstring::npos ? d.instance : d.instance.substr(i + 1))});
        }
        return out;
    }

    const char *driver_name() {
#ifdef _WIN32
        return "libusbK";
#else
        return "libusb-1.0";
#endif
    }

    const char *not_found_message() {
#ifdef _WIN32
        return "OmniCoreSDR with libusbK not found; run och-driver check";
#else
        return "OmniCoreSDR USB device not found (VID 1a86, PID 5537)";
#endif
    }

    DeviceLock::Result DeviceLock::acquire(const std::string &serial) {
        auto handle = std::make_shared<Handle>();
        handle->h = CreateMutexW(nullptr, FALSE, (L"Local\\OmniCoreHost_" + wide(serial)).c_str());
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        if (!handle->h) return Result::unavailable;
        held = std::move(handle);
        return existed ? Result::busy : Result::acquired;
    }
} // namespace och::transport
