#pragma once
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
    inline void module_anchor() {
    }

    inline std::wstring wide(const std::string &s, UINT cp = CP_UTF8) {
        if (s.empty())return {};
        int n = MultiByteToWideChar(cp, 0, s.data(), int(s.size()), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(cp, 0, s.data(), int(s.size()), w.data(), n);
        return w;
    }

    inline std::string utf8(const std::wstring &w, UINT cp = CP_UTF8) {
        if (w.empty())return {};
        int n = WideCharToMultiByte(cp, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, 0);
        WideCharToMultiByte(cp, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    inline std::wstring syserr(DWORD e) {
        wchar_t *p = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, e, 0, (LPWSTR) & p, 0, nullptr);
        std::wstring s = p ? p : L"Unknown error";
        if (p)LocalFree(p);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))s.pop_back();
        return L"[" + std::to_wstring(e) + L"] " + s;
    }

    inline double nowSec() {
        static const double inv = []() {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return 1.0 / double(f.QuadPart);
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return double(c.QuadPart) * inv;
    }

    struct Handle {
        HANDLE h = INVALID_HANDLE_VALUE;

        Handle() = default;

        explicit Handle(HANDLE v) : h(v) {
        }

        ~Handle() { if (h != INVALID_HANDLE_VALUE && h)CloseHandle(h); }

        Handle(const Handle &) = delete;

        Handle &operator=(const Handle &) = delete;
    };

    struct DeviceInfo {
        std::wstring path, label, instance;
    };

    inline std::wstring devProperty(HDEVINFO set, SP_DEVINFO_DATA &d, DWORD prop) {
        wchar_t b[2048]{};
        DWORD type = 0;
        if (SetupDiGetDeviceRegistryPropertyW(set, &d, prop, &type, (BYTE *) b, sizeof b, nullptr))return b;
        return {};
    }

    inline void registryGuids(HKEY key, std::set<std::wstring> &gs) {
        for (auto name: {L"DeviceInterfaceGUIDs", L"DeviceInterfaceGUID"}) {
            wchar_t b[4096]{};
            DWORD n = sizeof(b) - 2 * sizeof(wchar_t), type = 0;
            if (RegQueryValueExW(key, name, nullptr, &type, (BYTE *) b, &n) != ERROR_SUCCESS)continue;
            if (type != REG_SZ && type != REG_MULTI_SZ)continue;
            for (wchar_t *p = b; *p; p += wcslen(p) + 1) {
                GUID g;
                if (SUCCEEDED(CLSIDFromString(p, &g)))gs.insert(p);
                if (type == REG_SZ)break;
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
        std::set<std::wstring> gs = {
            L"{DEE824EF-729B-4A0E-9C14-B7117D33A817}", L"{12345678-1234-1234-1234-123456789ABC}",
            L"{80E62813-EBF8-4423-8DF0-DE6DE401356B}"
        };
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (set != INVALID_HANDLE_VALUE) {
            for (DWORD i = 0;; ++i) {
                SP_DEVINFO_DATA d{};
                d.cbSize = sizeof d;
                if (!SetupDiEnumDeviceInfo(set, i, &d))break;
                wchar_t inst[2048]{};
                if (!SetupDiGetDeviceInstanceIdW(set, &d, inst, 2048, nullptr) || !match(inst))continue;
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
                std::wstring rp = L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(inst) + L"\\Device Parameters";
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
            if (FAILED(CLSIDFromString(st.c_str(), &g)))continue;
            set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == INVALID_HANDLE_VALUE)continue;
            for (DWORD i = 0;; ++i) {
                SP_DEVICE_INTERFACE_DATA di{};
                di.cbSize = sizeof di;
                if (!SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &di))break;
                DWORD n = 0;
                SetupDiGetDeviceInterfaceDetailW(set, &di, nullptr, 0, &n, nullptr);
                if (!n)continue;
                std::vector<uint8_t> buf(n);
                auto detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *) buf.data();
                detail->cbSize = sizeof(*detail);
                SP_DEVINFO_DATA d{};
                d.cbSize = sizeof d;
                if (!SetupDiGetDeviceInterfaceDetailW(set, &di, detail, n, nullptr, &d))continue;
                wchar_t inst[2048]{};
                SetupDiGetDeviceInstanceIdW(set, &d, inst, 2048, nullptr);
                if (!match(inst) && !match(detail->DevicePath))continue;
                auto service = devProperty(set, d, SPDRP_SERVICE);
                if (_wcsicmp(service.c_str(), L"libusbK") != 0)continue;
                if (!instances.insert(inst).second)continue;
                auto name = devProperty(set, d, SPDRP_FRIENDLYNAME);
                if (name.empty())name = devProperty(set, d, SPDRP_DEVICEDESC);
                std::wstring serial = inst;
                size_t p = serial.find_last_of(L'\\');
                if (p != std::wstring::npos)serial = serial.substr(p + 1);
                out.push_back({detail->DevicePath, name + L"  |  " + serial, inst});
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
        bool ep1NormalSticky = false;
        HMODULE library = nullptr;

        HANDLE file = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE h = nullptr;
        std::map<UCHAR, WINUSB_PIPE_INFORMATION> pipes;
        std::wstring summary;
        ~Usb() { close(); }

        Usb() = default;

        Usb(const Usb &) = delete;

        Usb &operator=(const Usb &) = delete;

        void close() {
            if (h) { api.free(h); }
            h = nullptr;
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); }
            file = INVALID_HANDLE_VALUE;
            if (library) { FreeLibrary(library); }
            library = nullptr;
            api = UsbApi{};
            pipes.clear();
            ep1NormalSticky = false;
        }

        template<class T>
        void bind(T &fn, const char *name) {
            FARPROC proc = GetProcAddress(library, name);
            if (!proc)throw std::runtime_error(std::string("Missing libusbK API: ") + name);
            static_assert(sizeof(fn) == sizeof(proc));
            std::memcpy(&fn, &proc, sizeof(fn));
        }

        void open(const std::wstring &path) {
            close();
            try {
                // Resolve next to this library, then System32. Never search CWD/PATH.
                HMODULE owner = nullptr;
                wchar_t modulePath[32768]{};
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&module_anchor), &owner);
                DWORD pathLength = GetModuleFileNameW(owner, modulePath, 32768);
                if (pathLength && pathLength < 32768) {
                    std::wstring local(modulePath, pathLength);
                    local = local.substr(0, local.find_last_of(L"\\/") + 1) + L"libusbK.dll";
                    library = LoadLibraryExW(local.c_str(), nullptr,
                                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                }
                if (!library)library = LoadLibraryExW(L"libusbK.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!library)throw std::runtime_error(
                    utf8(L"Cannot load system libusbK.dll: " + syserr(GetLastError())));
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
                if (file == INVALID_HANDLE_VALUE)throw
                        std::runtime_error(utf8(L"CreateFile " + syserr(GetLastError())));
                if (!api.initialize(file, &h))throw std::runtime_error(
                    utf8(L"USB initialize " + syserr(GetLastError())));
                USB_INTERFACE_DESCRIPTOR d{};
                if (!api.queryInterface(h, 0, &d))throw std::runtime_error("Cannot query interface");
                for (UCHAR i = 0; i < d.bNumEndpoints; ++i) {
                    WINUSB_PIPE_INFORMATION pi{};
                    if (api.queryPipe(h, 0, i, &pi) && pi.PipeType == UsbdPipeTypeBulk)pipes[pi.PipeId] = pi;
                }
                if (!pipes.count(0x81) || !pipes.count(0x01))throw std::runtime_error("EP1 IN/OUT not found");
                summary = L"libusbK (TX parallel) | Interface " + std::to_wstring(d.bInterfaceNumber) + L" | " + (
                              pipes[0x81].MaximumPacketSize == 1024
                                  ? L"SuperSpeed"
                                  : (pipes[0x81].MaximumPacketSize == 512 ? L"High-Speed" : L"Full-Speed")) +
                          L" | Bulk pipes: " + std::to_wstring(pipes.size());
            } catch (...) {
                close();
                throw;
            }
        }

        void parallelLimit(UCHAR ep, ULONG depth) {
            if (depth < 1 || depth > 128)throw std::runtime_error("Invalid libusbK parallel depth");
            constexpr ULONG simulParallelRequests = 0x30;
            if (!api.setPolicy(h, ep, simulParallelRequests, sizeof depth, &depth))throw std::runtime_error(
                utf8(L"Parallel queue: " + syserr(GetLastError())));
            ULONG actual = 0, n = sizeof actual;
            if (!api.getPolicy(h, ep, simulParallelRequests, &n, &actual) || actual != depth)throw std::runtime_error(
                "Parallel queue policy verification failed");
        }

        bool has(UCHAR ep) const { return h && pipes.count(ep); }

        ULONG maximum(UCHAR ep) {
            ULONG value = 1024 * 1024, n = sizeof value;
            if (!api.getPolicy(h, ep, MAXIMUM_TRANSFER_SIZE, &n, &value) || !value)value = 65536;
            return value;
        }

        void policy(UCHAR ep, ULONG which, UCHAR v) {
            if (!api.setPolicy(h, ep, which, sizeof v, &v))throw std::runtime_error(
                utf8(L"Pipe policy " + syserr(GetLastError())));
        }

        void timeout(UCHAR ep, ULONG v) {
            if (!api.setPolicy(h, ep, PIPE_TRANSFER_TIMEOUT, sizeof v, &v))throw std::runtime_error(
                utf8(L"Pipe timeout " + syserr(GetLastError())));
        }
    };

    struct MappedFile {
        Handle file, map;
        const uint8_t *data = nullptr;
        uint64_t size = 0;

        explicit MappedFile(const std::wstring &path) {
            file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.h == INVALID_HANDLE_VALUE)throw std::runtime_error(utf8(syserr(GetLastError())));
            LARGE_INTEGER n;
            if (!GetFileSizeEx(file.h, &n) || n.QuadPart <= 0)throw std::runtime_error("File is empty or inaccessible");
            size = uint64_t(n.QuadPart);
            map.h = CreateFileMappingW(file.h, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!map.h)throw std::runtime_error(utf8(syserr(GetLastError())));
            data = (const uint8_t *) MapViewOfFile(map.h, FILE_MAP_READ, 0, 0, 0);
            if (!data)throw std::runtime_error(utf8(syserr(GetLastError())));
        }

        ~MappedFile() { if (data)UnmapViewOfFile(data); }
    };
} // namespace och::transport
