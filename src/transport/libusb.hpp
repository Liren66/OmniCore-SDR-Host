// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "linux_events.hpp"
#include <libusb.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <codecvt>
#include <cstring>
#include <deque>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

using WINUSB_INTERFACE_HANDLE = void *;
constexpr unsigned UsbdPipeTypeBulk = 2;

struct WINUSB_PIPE_INFORMATION {
    unsigned PipeType = 2;
    UCHAR PipeId = 0;
    uint16_t MaximumPacketSize = 0;
};

struct WINUSB_SETUP_PACKET {
    UCHAR RequestType = 0, Request = 0;
    uint16_t Value = 0, Index = 0, Length = 0;
};

constexpr ULONG RAW_IO = 1, ALLOW_PARTIAL_READS = 2, AUTO_FLUSH = 3, IGNORE_SHORT_PACKETS = 4,
        SHORT_PACKET_TERMINATE = 5, PIPE_TRANSFER_TIMEOUT = 6, MAXIMUM_TRANSFER_SIZE = 7;

namespace och::transport {
    inline std::wstring wide(const std::string &s) {
        return std::wstring_convert<std::codecvt_utf8<wchar_t> >{}.from_bytes(s);
    }

    inline std::string utf8(const std::wstring &s) {
        return std::wstring_convert<std::codecvt_utf8<wchar_t> >{}.to_bytes(s);
    }

    // libusb failures occupy a separate range from internal completion errors.
    inline DWORD usb_error(int rc) { return static_cast<DWORD>(10000 - rc); }

    inline std::wstring syserr(DWORD e) {
        if (e >= 10000)
            return wide(std::string(libusb_error_name(10000 - int(e))) +
                        (e == usb_error(LIBUSB_ERROR_NO_MEM)
                             ? "; USB queue allocation failed: check usbcore.usbfs_memory_mb"
                             : ""));
        if (e == ERROR_SEM_TIMEOUT) return L"USB transfer timed out";
        if (e == ERROR_OPERATION_ABORTED) return L"USB transfer cancelled";
        if (e == ERROR_IO_INCOMPLETE) return L"USB transfer is pending";
        return wide("error " + std::to_string(e));
    }

    struct Handle {
        HANDLE h = nullptr;

        Handle() = default;

        explicit Handle(HANDLE v) : h(v) {
        }

        ~Handle() { CloseHandle(h); }

        Handle(const Handle &) = delete;

        Handle &operator=(const Handle &) = delete;
    };

    struct DeviceInfo {
        std::wstring path, instance;
    };

    struct UsbApi {
        BOOL (*free)(WINUSB_INTERFACE_HANDLE) = nullptr;

        BOOL (*read)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED) = nullptr;

        BOOL (*write)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED) = nullptr;

        BOOL (*result)(WINUSB_INTERFACE_HANDLE, LPOVERLAPPED, PULONG, BOOL) = nullptr;

        BOOL (*abort)(WINUSB_INTERFACE_HANDLE, UCHAR) = nullptr;

        BOOL (*control)(WINUSB_INTERFACE_HANDLE, WINUSB_SETUP_PACKET, PUCHAR, ULONG, PULONG, LPOVERLAPPED) = nullptr;

        BOOL (*setPolicy)(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, ULONG, PVOID) = nullptr;

        BOOL (*getPolicy)(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, PULONG, PVOID) = nullptr;
    };

    struct Usb;

    struct LinuxTransfer {
        libusb_transfer *native = libusb_alloc_transfer(0);
        Usb *owner = nullptr;
        HANDLE event = nullptr;
        bool completed = false;

        ~LinuxTransfer() {
            if (native) libusb_free_transfer(native);
        }
    };

    struct Usb {
        UsbApi api{};
        WINUSB_INTERFACE_HANDLE h = nullptr;
        HANDLE file = nullptr;
        std::map<UCHAR, WINUSB_PIPE_INFORMATION> pipes;
        libusb_context *context = nullptr;
        libusb_device_handle *device = nullptr;
        int interface_number = -1;
        std::atomic<bool> stopping{false};
        std::thread events;
        std::mutex mutex;
        std::condition_variable drained;
        std::set<LinuxTransfer *> active;
        std::array<ULONG, 256> timeouts{};
        bool control_stopping = false;

        Usb() = default;

        Usb(const Usb &) = delete;

        Usb &operator=(const Usb &) = delete;

        ~Usb() { close(); }

        static void LIBUSB_CALL completion(libusb_transfer *t) {
            auto *r = static_cast<LinuxTransfer *>(t->user_data);
            auto &u = *r->owner;
            std::lock_guard<std::mutex> lock(u.mutex);
            r->completed = true;
            u.active.erase(r);
            SetEvent(r->event);
            u.drained.notify_all();
        }

        static BOOL submit(WINUSB_INTERFACE_HANDLE handle, UCHAR ep, PUCHAR bytes, ULONG size, PULONG count,
                           LPOVERLAPPED ov) {
            auto &u = *static_cast<Usb *>(handle);
            if (!ov || !ov->hEvent) {
                SetLastError(usb_error(LIBUSB_ERROR_INVALID_PARAM));
                return FALSE;
            }
            if (!ov->transfer) ov->transfer = std::make_shared<LinuxTransfer>();
            auto &r = *ov->transfer;
            if (!r.native) {
                SetLastError(usb_error(LIBUSB_ERROR_NO_MEM));
                return FALSE;
            }
            std::lock_guard<std::mutex> lock(u.mutex);
            if (ep == 0x82 && u.control_stopping) {
                SetLastError(ERROR_OPERATION_ABORTED);
                return FALSE;
            }
            if (u.active.count(&r)) {
                SetLastError(usb_error(LIBUSB_ERROR_BUSY));
                return FALSE;
            }
            r.owner = &u;
            r.event = ov->hEvent;
            r.completed = false;
            libusb_fill_bulk_transfer(r.native, u.device, ep, bytes, int(size), completion, &r, u.timeouts[ep]);
            u.active.insert(&r);
            int rc = libusb_submit_transfer(r.native);
            if (rc) {
                u.active.erase(&r);
                SetLastError(usb_error(rc));
                return FALSE;
            }
            if (count) *count = 0;
            SetLastError(ERROR_IO_PENDING);
            return FALSE;
        }

        static BOOL result(WINUSB_INTERFACE_HANDLE handle, LPOVERLAPPED ov, PULONG count, BOOL wait) {
            auto &u = *static_cast<Usb *>(handle);
            if (!ov || !ov->transfer) {
                SetLastError(ERROR_NOT_FOUND);
                return FALSE;
            }
            if (wait) WaitForMultipleObjects(1, &ov->hEvent, FALSE, INFINITE);
            std::lock_guard<std::mutex> lock(u.mutex);
            auto &r = *ov->transfer;
            if (!r.completed) {
                SetLastError(ERROR_IO_INCOMPLETE);
                return FALSE;
            }
            auto *t = r.native;
            if (count) *count = static_cast<ULONG>(t->actual_length);
            // A timed-out IN request may contain complete control lines or stale
            // stream data. Preserve those bytes instead of silently dropping them.
            if (t->status == LIBUSB_TRANSFER_COMPLETED ||
                (t->status == LIBUSB_TRANSFER_TIMED_OUT && (t->endpoint & 0x80) && t->actual_length))
                return TRUE;
            DWORD error = usb_error(LIBUSB_ERROR_IO);
            if (t->status == LIBUSB_TRANSFER_TIMED_OUT)
                error = ERROR_SEM_TIMEOUT;
            else if (t->status == LIBUSB_TRANSFER_CANCELLED)
                error = ERROR_OPERATION_ABORTED;
            else if (t->status == LIBUSB_TRANSFER_NO_DEVICE)
                error = usb_error(LIBUSB_ERROR_NO_DEVICE);
            else if (t->status == LIBUSB_TRANSFER_STALL)
                error = usb_error(LIBUSB_ERROR_PIPE);
            else if (t->status == LIBUSB_TRANSFER_OVERFLOW)
                error = usb_error(LIBUSB_ERROR_OVERFLOW);
            SetLastError(error);
            return FALSE;
        }

        static BOOL abort(WINUSB_INTERFACE_HANDLE handle, UCHAR ep) {
            auto &u = *static_cast<Usb *>(handle);
            std::lock_guard<std::mutex> lock(u.mutex);
            for (auto *r: u.active)
                if (r->native->endpoint == ep) libusb_cancel_transfer(r->native);
            return TRUE;
        }

        static BOOL control(WINUSB_INTERFACE_HANDLE handle, WINUSB_SETUP_PACKET p, PUCHAR data, ULONG size,
                            PULONG count, LPOVERLAPPED) {
            auto &u = *static_cast<Usb *>(handle);
            int rc = libusb_control_transfer(u.device, p.RequestType, p.Request, p.Value, p.Index, data,
                                             static_cast<uint16_t>(std::min<ULONG>(size, p.Length)), 2000);
            if (rc < 0) {
                SetLastError(usb_error(rc));
                return FALSE;
            }
            if (count) *count = static_cast<ULONG>(rc);
            return TRUE;
        }

        static BOOL set_policy(WINUSB_INTERFACE_HANDLE handle, UCHAR ep, ULONG key, ULONG size, PVOID value) {
            auto &u = *static_cast<Usb *>(handle);
            if (key == PIPE_TRANSFER_TIMEOUT && size == sizeof(ULONG)) {
                std::lock_guard<std::mutex> lock(u.mutex);
                memcpy(&u.timeouts[ep], value, size);
            }
            // libusb already delivers partial reads, with no auto flush, no short
            // packet padding, and all submitted transfers in endpoint order.
            return TRUE;
        }

        static BOOL get_policy(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG key, PULONG size, PVOID value) {
            if (key != MAXIMUM_TRANSFER_SIZE || *size < sizeof(ULONG)) return FALSE;
            ULONG maximum = 1048576;
            memcpy(value, &maximum, sizeof maximum);
            *size = sizeof maximum;
            return TRUE;
        }

        void close() {
            if (device) {
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    for (auto *r: active) libusb_cancel_transfer(r->native);
                    drained.wait(lock, [&] { return active.empty(); });
                }
                stopping = true;
                libusb_interrupt_event_handler(context);
                if (events.joinable()) events.join();
                if (interface_number >= 0) libusb_release_interface(device, interface_number);
                libusb_close(device);
                device = nullptr;
                interface_number = -1;
            }
            if (h && api.free) api.free(h);
            h = nullptr;
            if (context) libusb_exit(context);
            context = nullptr;
            pipes.clear();
            api = {};
        }

        void open(const std::wstring &path);

        void stop_control() {
            std::lock_guard<std::mutex> lock(mutex);
            control_stopping = true;
            for (auto *r: active)
                if (r->native->endpoint == 0x82) libusb_cancel_transfer(r->native);
        }

        bool has(UCHAR ep) const { return h && pipes.count(ep); }

        ULONG maximum(UCHAR ep) {
            ULONG value = 1048576, n = sizeof value;
            if (!api.getPolicy(h, ep, MAXIMUM_TRANSFER_SIZE, &n, &value)) value = 65536;
            return value;
        }

        void policy(UCHAR ep, ULONG key, UCHAR v) {
            if (!api.setPolicy(h, ep, key, sizeof v, &v)) throw std::runtime_error("USB pipe policy failed");
        }

        void timeout(UCHAR ep, ULONG v) {
            if (!api.setPolicy(h, ep, PIPE_TRANSFER_TIMEOUT, sizeof v, &v))
                throw std::runtime_error("USB timeout policy failed");
        }

        void parallelLimit(UCHAR, ULONG depth) {
            if (depth < 1 || depth > 128) throw std::runtime_error("Invalid USB parallel depth");
        }
    };

    struct Context {
        libusb_context *p = nullptr;

        Context() {
            int rc = libusb_init(&p);
            if (rc) throw std::runtime_error(libusb_error_name(rc));
        }

        ~Context() { libusb_exit(p); }
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
    };

    inline std::wstring device_path(libusb_device *d) {
        return std::to_wstring(libusb_get_bus_number(d)) + L":" + std::to_wstring(libusb_get_device_address(d));
    }

    inline std::vector<DeviceInfo> scanDevices(unsigned vid, unsigned pid) {
        Context ctx;
        DeviceList list(ctx.p);
        std::vector<DeviceInfo> found;
        for (ssize_t i = 0; i < list.count; ++i) {
            auto *d = list.p[i];
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(d, &desc) || desc.idVendor != vid || desc.idProduct != pid) continue;
            std::wstring serial;
            libusb_device_handle *h = nullptr;
            if (!libusb_open(d, &h)) {
                unsigned char text[256]{};
                int n = desc.iSerialNumber
                            ? libusb_get_string_descriptor_ascii(h, desc.iSerialNumber, text, sizeof text)
                            : 0;
                if (n > 0) serial = wide(std::string(reinterpret_cast<char *>(text), n));
                libusb_close(h);
            }
            if (serial.empty()) {
                uint8_t ports[8]{};
                int n = libusb_get_port_numbers(d, ports, sizeof ports);
                serial = L"usb-" + std::to_wstring(libusb_get_bus_number(d));
                for (int j = 0; j < n; ++j) serial += L"-" + std::to_wstring(ports[j]);
            }
            found.push_back({device_path(d), serial});
        }
        return found;
    }

    inline void Usb::open(const std::wstring &path) {
        close();
        try {
            int rc = libusb_init(&context);
            if (rc) throw std::runtime_error(libusb_error_name(rc));
            DeviceList list(context);
            for (ssize_t i = 0; i < list.count; ++i)
                if (device_path(list.p[i]) == path) {
                    rc = libusb_open(list.p[i], &device);
                    if (rc)
                        throw std::runtime_error(
                            std::string("Cannot open OmniCore USB: ") + libusb_error_name(rc) +
                            (rc == LIBUSB_ERROR_ACCESS
                                 ? "; install packaging/linux/60-omnicore.rules and replug the device"
                                 : ""));
                    break;
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
            for (unsigned i = 0; i < raw->bNumInterfaces; ++i) {
                const auto &interface = raw->interface[i];
                for (int j = 0; j < interface.num_altsetting; ++j) {
                    const auto &alt = interface.altsetting[j];
                    if (alt.bAlternateSetting != 0) continue;
                    std::map<UCHAR, WINUSB_PIPE_INFORMATION> candidate;
                    for (unsigned k = 0; k < alt.bNumEndpoints; ++k) {
                        const auto &e = alt.endpoint[k];
                        if ((e.bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_BULK)
                            candidate[e.bEndpointAddress] = {UsbdPipeTypeBulk, e.bEndpointAddress, e.wMaxPacketSize};
                    }
                    if (candidate.count(0x81) && candidate.count(0x01) && candidate.count(0x82) &&
                        candidate.count(0x02)) {
                        selected = alt.bInterfaceNumber;
                        pipes = std::move(candidate);
                        break;
                    }
                }
                if (selected >= 0) break;
            }
            if (selected < 0) throw std::runtime_error("OmniCore bulk endpoints 01/81/02/82 not found");
            rc = libusb_claim_interface(device, selected);
            if (rc) throw std::runtime_error(std::string("Cannot claim OmniCore interface: ") + libusb_error_name(rc));
            interface_number = selected;
            h = this;
            api.read = api.write = submit;
            api.result = result;
            api.abort = abort;
            api.control = control;
            api.setPolicy = set_policy;
            api.getPolicy = get_policy;
            stopping = false;
            control_stopping = false;
            events = std::thread([this] {
                while (!stopping) {
                    timeval timeout{0, 50000};
                    libusb_handle_events_timeout_completed(context, &timeout, nullptr);
                }
            });
        } catch (...) {
            close();
            throw;
        }
    }
} // namespace och::transport
