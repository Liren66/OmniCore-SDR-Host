// Windows backend of the transport interface (link.hpp): libusbK, loaded at run time, with
// overlapped transfers. Enumeration through SetupAPI; the cross-process lock is a named mutex.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../link.hpp"
#include <windows.h>
#include <setupapi.h>
#include <winusb.h> // ABI-compatible types only; no WinUSB imports or backend.
#include <usbspec.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <stdexcept>

// Adapted from the device owner's OminiCore_SDR_USB_Test transport.
namespace och::transport {
    namespace {
        inline void module_anchor() {}

        inline std::wstring wide(const std::string &s, UINT cp = CP_UTF8) {
            if (s.empty()) return {};
            int n = MultiByteToWideChar(cp, 0, s.data(), int(s.size()), nullptr, 0);
            std::wstring w(n, 0);
            MultiByteToWideChar(cp, 0, s.data(), int(s.size()), w.data(), n);
            return w;
        }

        inline std::string utf8(const std::wstring &w, UINT cp = CP_UTF8) {
            if (w.empty()) return {};
            int n = WideCharToMultiByte(cp, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
            std::string s(n, 0);
            WideCharToMultiByte(cp, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
            return s;
        }

        inline std::wstring syserr(DWORD e) {
            wchar_t *p = nullptr;
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, e, 0, (LPWSTR) &p, 0, nullptr);
            std::wstring s = p ? p : L"Unknown error";
            if (p) LocalFree(p);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return L"[" + std::to_wstring(e) + L"] " + s;
        }

        struct Handle {
            HANDLE h = INVALID_HANDLE_VALUE;

            Handle() = default;

            explicit Handle(HANDLE v) : h(v) {}

            ~Handle() {
                if (h != INVALID_HANDLE_VALUE && h) CloseHandle(h);
            }

            Handle(const Handle &) = delete;

            Handle &operator=(const Handle &) = delete;
        };

        struct DeviceInfo {
            std::wstring path, instance;
        };

        inline std::wstring devProperty(HDEVINFO set, SP_DEVINFO_DATA &d, DWORD prop) {
            wchar_t b[2048]{};
            DWORD type = 0;
            if (SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, (BYTE *) b, sizeof b, nullptr)) return b;
            return {};
        }

        inline void registryGuids(HKEY key, std::set<std::wstring> &gs) {
            for (auto name: {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"}) {
                wchar_t b[4096]{};
                DWORD n = sizeof(b) - 2 * sizeof(wchar_t), type = 0;
                if (RegQueryValueExW(key, name, nullptr, &type, (BYTE *) b, &n) != ERROR_SUCCESS) continue;
                if (type != REG_SZ && type != REG_MULTI_SZ) continue;
                for (wchar_t *p = b; *p; p += wcslen(p) + 1) {
                    GUID g;
                    if (SUCCEEDED(CLSIDFromString(p, &g))) gs.insert(p);
                    if (type == REG_SZ) break;
                }
            }
        }

        inline std::vector<DeviceInfo> scanDevices(unsigned vid, unsigned pid) {
            wchar_t token[64];
            swprintf(token, 64, L"VID_%04X&PID_%04X", vid, pid);
            auto match = [&](std::wstring s) {
                std::transform(s.begin(), s.end(), s.begin(), ::towupper);
                return s.find(token) != std::wstring::npos;
            };
            std::set<std::wstring> gs = {L"{DEE824EF-729B-4A0E-9C14-B7117D33A817}",
                                         L"{12345678-1234-1234-1234-123456789ABC}",
                                         L"{80E62813-EBF8-4423-8DF0-DE6DE401356B}"};
            HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
            if (set != INVALID_HANDLE_VALUE) {
                for (DWORD i = 0;; ++i) {
                    SP_DEVINFO_DATA d{};
                    d.cbSize = sizeof d;
                    if (!SetupDiEnumDeviceInfo(set, i, &d)) break;
                    wchar_t inst[2048]{};
                    if (!SetupDiGetDeviceInstanceIdW(set, &d, inst, 2048, nullptr) || !match(inst)) continue;
                    HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
                    if (k != INVALID_HANDLE_VALUE) {
                        registryGuids(k, gs);
                        HKEY sub;
                        if (RegOpenKeyExW(k, L"Device Parameters", 0, KEY_READ, &sub) == ERROR_SUCCESS) {
                            registryGuids(sub, gs);
                            RegCloseKey(sub);
                        }
                        RegCloseKey(k);
                    }
                    std::wstring rp =
                            L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(inst) + L"\\Device Parameters";
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, rp.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
                        registryGuids(k, gs);
                        RegCloseKey(k);
                    }
                }
                SetupDiDestroyDeviceInfoList(set);
            }
            std::vector<DeviceInfo> out;
            std::set<std::wstring> instances;
            for (const auto &st: gs) {
                GUID g;
                if (FAILED(CLSIDFromString(st.c_str(), &g))) continue;
                set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
                if (set == INVALID_HANDLE_VALUE) continue;
                for (DWORD i = 0;; ++i) {
                    SP_DEVICE_INTERFACE_DATA di{};
                    di.cbSize = sizeof di;
                    if (!SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &di)) break;
                    DWORD n = 0;
                    SetupDiGetDeviceInterfaceDetailW(set, &di, nullptr, 0, &n, nullptr);
                    if (!n) continue;
                    std::vector<uint8_t> buf(n);
                    auto detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *) buf.data();
                    detail->cbSize = sizeof(*detail);
                    SP_DEVINFO_DATA d{};
                    d.cbSize = sizeof d;
                    if (!SetupDiGetDeviceInterfaceDetailW(set, &di, detail, n, nullptr, &d)) continue;
                    wchar_t inst[2048]{};
                    SetupDiGetDeviceInstanceIdW(set, &d, inst, 2048, nullptr);
                    if (!match(inst) && !match(detail->DevicePath)) continue;
                    auto service = devProperty(set, d, SPDRP_SERVICE);
                    if (_wcsicmp(service.c_str(), L"libusbK") != 0) continue;
                    if (!instances.insert(inst).second) continue;
                    out.push_back({detail->DevicePath, inst});
                }
                SetupDiDestroyDeviceInfoList(set);
            }
            return out;
        }

        struct UsbApi {
            decltype(&WinUsb_Initialize) initialize = nullptr;
            decltype(&WinUsb_Free) free = nullptr;
            decltype(&WinUsb_QueryInterfaceSettings) queryInterface = nullptr;
            decltype(&WinUsb_QueryPipe) queryPipe = nullptr;
            decltype(&WinUsb_QueryDeviceInformation) queryDevice = nullptr;
            decltype(&WinUsb_ControlTransfer) control = nullptr;
            decltype(&WinUsb_ResetPipe) reset = nullptr;
            decltype(&WinUsb_ReadPipe) read = nullptr;
            decltype(&WinUsb_WritePipe) write = nullptr;
            decltype(&WinUsb_GetOverlappedResult) result = nullptr;
            decltype(&WinUsb_SetPipePolicy) setPolicy = nullptr;
            decltype(&WinUsb_GetPipePolicy) getPolicy = nullptr;
            decltype(&WinUsb_AbortPipe) abort = nullptr;
        };

        struct Usb {
            UsbApi api;
            HMODULE library = nullptr;

            HANDLE file = INVALID_HANDLE_VALUE;
            WINUSB_INTERFACE_HANDLE h = nullptr;
            std::map<UCHAR, WINUSB_PIPE_INFORMATION> pipes;
            ~Usb() { close(); }

            Usb() = default;

            Usb(const Usb &) = delete;

            Usb &operator=(const Usb &) = delete;

            void close() {
                if (h) {
                    api.free(h);
                }
                h = nullptr;
                if (file != INVALID_HANDLE_VALUE) {
                    CloseHandle(file);
                }
                file = INVALID_HANDLE_VALUE;
                if (library) {
                    FreeLibrary(library);
                }
                library = nullptr;
                api = UsbApi{};
                pipes.clear();
            }

            template<class T>
            void bind(T &fn, const char *name) {
                FARPROC proc = GetProcAddress(library, name);
                if (!proc) throw std::runtime_error(std::string("Missing libusbK API: ") + name);
                static_assert(sizeof(fn) == sizeof(proc));
                std::memcpy(&fn, &proc, sizeof(fn));
            }

            void open(const std::wstring &path) {
                close();
                try {
                    // Resolve next to this library, then System32. Never search CWD/PATH.
                    HMODULE owner = nullptr;
                    wchar_t modulePath[32768]{};
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(&module_anchor), &owner);
                    DWORD pathLength = GetModuleFileNameW(owner, modulePath, 32768);
                    if (pathLength && pathLength < 32768) {
                        std::wstring local(modulePath, pathLength);
                        local = local.substr(0, local.find_last_of(L"\\/") + 1) + L"libusbK.dll";
                        library = LoadLibraryExW(local.c_str(), nullptr,
                                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                    }
                    if (!library) library = LoadLibraryExW(L"libusbK.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                    if (!library)
                        throw std::runtime_error(utf8(L"Cannot load system libusbK.dll: " + syserr(GetLastError())));
                    bind(api.initialize, "UsbK_Initialize");
                    bind(api.free, "UsbK_Free");
                    bind(api.queryInterface, "UsbK_QueryInterfaceSettings");
                    bind(api.queryPipe, "UsbK_QueryPipe");
                    bind(api.queryDevice, "UsbK_QueryDeviceInformation");
                    bind(api.control, "UsbK_ControlTransfer");
                    bind(api.reset, "UsbK_ResetPipe");
                    bind(api.read, "UsbK_ReadPipe");
                    bind(api.write, "UsbK_WritePipe");
                    bind(api.result, "UsbK_GetOverlappedResult");
                    bind(api.setPolicy, "UsbK_SetPipePolicy");
                    bind(api.getPolicy, "UsbK_GetPipePolicy");
                    bind(api.abort, "UsbK_AbortPipe");
                    file = CreateFileW(path.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
                    if (file == INVALID_HANDLE_VALUE)
                        throw std::runtime_error(utf8(L"CreateFile " + syserr(GetLastError())));
                    if (!api.initialize(file, &h))
                        throw std::runtime_error(utf8(L"USB initialize " + syserr(GetLastError())));
                    USB_INTERFACE_DESCRIPTOR d{};
                    if (!api.queryInterface(h, 0, &d)) throw std::runtime_error("Cannot query interface");
                    for (UCHAR i = 0; i < d.bNumEndpoints; ++i) {
                        WINUSB_PIPE_INFORMATION pi{};
                        if (api.queryPipe(h, 0, i, &pi) && pi.PipeType == UsbdPipeTypeBulk) pipes[pi.PipeId] = pi;
                    }
                    if (!pipes.count(0x81) || !pipes.count(0x01)) throw std::runtime_error("EP1 IN/OUT not found");
                } catch (...) {
                    close();
                    throw;
                }
            }

            void parallelLimit(UCHAR ep, ULONG depth) {
                if (depth < 1 || depth > 128) throw std::runtime_error("Invalid libusbK parallel depth");
                constexpr ULONG simulParallelRequests = 0x30;
                if (!api.setPolicy(h, ep, simulParallelRequests, sizeof depth, &depth))
                    throw std::runtime_error(utf8(L"Parallel queue: " + syserr(GetLastError())));
                ULONG actual = 0, n = sizeof actual;
                if (!api.getPolicy(h, ep, simulParallelRequests, &n, &actual) || actual != depth)
                    throw std::runtime_error("Parallel queue policy verification failed");
            }

            bool has(UCHAR ep) const { return h && pipes.count(ep); }

            ULONG maximum(UCHAR ep) {
                ULONG value = 1024 * 1024, n = sizeof value;
                if (!api.getPolicy(h, ep, MAXIMUM_TRANSFER_SIZE, &n, &value) || !value) value = 65536;
                return value;
            }

            void policy(UCHAR ep, ULONG which, UCHAR v) {
                if (!api.setPolicy(h, ep, which, sizeof v, &v))
                    throw std::runtime_error(utf8(L"Pipe policy " + syserr(GetLastError())));
            }

            void timeout(UCHAR ep, ULONG v) {
                if (!api.setPolicy(h, ep, PIPE_TRANSFER_TIMEOUT, sizeof v, &v))
                    throw std::runtime_error(utf8(L"Pipe timeout " + syserr(GetLastError())));
            }
        };

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

        // SampleQueue on overlapped libusbK transfers: one event per transfer plus the wake event.
        class LibusbKQueue final : public SampleQueue {
            struct Slot : Transfer {
                OVERLAPPED ov{};
                Handle event;
                std::unique_ptr<uint8_t[]> bytes;
                bool pending = false;

                explicit Slot(size_t n)
                    : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), bytes(std::make_unique<uint8_t[]>(n)) {
                    if (!event.h) throw Error("cannot create USB event");
                    ov.hEvent = event.h;
                    data = bytes.get();
                    capacity = n;
                    memset(data, 0, n);
                }
            };

        public:
            LibusbKQueue(Usb &u, Pipe pipe, size_t transfer_bytes, unsigned depth)
                : usb(u), ep(endpoint(pipe)), wake_event(CreateEventW(nullptr, TRUE, FALSE, nullptr)), active(depth) {
                if (!wake_event.h) throw Error("stream stop event allocation failed");
                slots.reserve(depth);
                for (unsigned i = 0; i < depth; ++i) slots.push_back(std::make_unique<Slot>(transfer_bytes));
            }

            size_t size() const override { return slots.size(); }

            Transfer &slot(size_t index) override { return *slots[index]; }

            size_t max_transfer_bytes() override { return usb.maximum(ep); }

            void start() override {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
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
                const BOOL ok = (ep & 0x80)
                                        ? usb.api.read(usb.h, ep, s.data, static_cast<ULONG>(s.used), nullptr, &s.ov)
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
                        CancelIoEx(usb.file, &s->ov);
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

        class LibusbKLink final : public Link {
        public:
            explicit LibusbKLink(const DeviceId &id) { usb.open(wide(id.path)); }

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
                // Short timed reads: the control thread sees its stop flag within this time.
                constexpr ULONG read_timeout = 50;
                prepare(Pipe::control_in);
                return bulk(endpoint(Pipe::control_in), data, size, read_timeout);
            }

            // Nothing to cancel: read_control() returns within its 50 ms timeout.
            void shutdown_control() noexcept override {}

            void reset_sample_pipes() override {
                usb.policy(endpoint(Pipe::sample_in), RAW_IO, FALSE);
                usb.policy(endpoint(Pipe::sample_in), ALLOW_PARTIAL_READS, TRUE);
            }

            std::unique_ptr<SampleQueue> open_samples(Pipe p, size_t transfer_bytes, unsigned depth) override {
                return std::make_unique<LibusbKQueue>(usb, p, transfer_bytes, depth);
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
    } // namespace

    std::vector<DeviceId> enumerate() {
        std::vector<DeviceId> out;
        for (const auto &d: scanDevices(0x1a86, 0x5537)) {
            // The serial is the last component of the device instance id.
            const auto i = d.instance.find_last_of(L'\\');
            out.push_back({utf8(d.path), utf8(i == std::wstring::npos ? d.instance : d.instance.substr(i + 1))});
        }
        return out;
    }

    std::unique_ptr<Link> open(const DeviceId &id) { return std::make_unique<LibusbKLink>(id); }

    const char *driver_name() { return "libusbK"; }

    const char *not_found_message() { return "OmniCoreSDR with libusbK not found; run och-driver check"; }

    // A named mutex in the session namespace: it exists while any process holds a handle to it.
    DeviceLock::Result DeviceLock::acquire(const std::string &serial) {
        auto handle = std::make_shared<Handle>();
        handle->h = CreateMutexW(nullptr, FALSE, (L"Local\\OmniCoreHost_" + wide(serial)).c_str());
        const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
        if (!handle->h) return Result::unavailable;
        held = std::move(handle);
        return existed ? Result::busy : Result::acquired;
    }
} // namespace och::transport
