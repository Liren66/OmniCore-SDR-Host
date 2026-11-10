// OmniCore Host USB driver installer. Only USB\VID_1A86&PID_5537 is supported.
// libwdi is dynamically linked and distributed with its source and LGPL notices.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include "libwdi.h"

namespace fs = std::filesystem;
constexpr unsigned short vid = 0x1A86, pid = 0x5537;
constexpr const char *kernel_sha256 = "1df66ff22e2eaec27180756d90926ca5b07e8bcf6b0e4e3c56471e63a3a05fa6";
void log(const std::string &s) { std::cout << s << std::endl; }
std::string utf8(const std::wstring &s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
fs::path executable() {
    std::vector<wchar_t> s(32768);
    auto n = GetModuleFileNameW(nullptr, s.data(), static_cast<DWORD>(s.size()));
    if (!n || n == s.size()) throw std::runtime_error("Cannot locate installer executable");
    return std::wstring(s.data(), n);
}
std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &p))) throw std::runtime_error("Cannot locate Windows data folder");
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}
bool elevated() {
    HANDLE t = nullptr;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    bool ok = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) != 0;
    CloseHandle(t);
    return ok && e.TokenIsElevated;
}
bool target(const wdi_device_info *d) { return d && d->vid == vid && d->pid == pid && !d->is_composite; }
bool ready(const wdi_device_info *d) { return d->driver && _stricmp(d->driver, "libusbK") == 0; }
std::string version(UINT64 v) {
    return std::to_string((v >> 48) & 65535) + "." + std::to_string((v >> 32) & 65535) + "." +
           std::to_string((v >> 16) & 65535) + "." + std::to_string(v & 65535);
}

struct Wdi {
    HMODULE lib = nullptr;
    decltype(&wdi_create_list) list = nullptr;
    decltype(&wdi_destroy_list) free_list = nullptr;
    decltype(&wdi_prepare_driver) prepare = nullptr;
    decltype(&wdi_install_driver) install = nullptr;
    decltype(&wdi_strerror) error = nullptr;
    decltype(&wdi_set_log_level) level = nullptr;
    Wdi() {
        auto p = executable().parent_path() / L"libwdi.dll";
        lib = LoadLibraryExW(p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!lib)
            throw std::runtime_error("Cannot load bundled libwdi.dll (Windows error " + std::to_string(GetLastError()) +
                                     ")");
#define API(field, name)                                                                                               \
    {                                                                                                                  \
        auto proc = GetProcAddress(lib, #name);                                                                        \
        static_assert(sizeof(proc) == sizeof(field));                                                                  \
        std::memcpy(&field, &proc, sizeof(field));                                                                     \
        if (!field) throw std::runtime_error("Missing libwdi API: " #name);                                            \
    }
        API(list, wdi_create_list);
        API(free_list, wdi_destroy_list);
        API(prepare, wdi_prepare_driver);
        API(install, wdi_install_driver);
        API(error, wdi_strerror);
        API(level, wdi_set_log_level);
#undef API
        level(WDI_LOG_LEVEL_INFO);
    }
    ~Wdi() {
        if (lib) FreeLibrary(lib);
    }
};
struct DeviceList {
    Wdi &w;
    wdi_device_info *head = nullptr;
    DeviceList(Wdi &api) : w(api) {
        wdi_options_create_list o{};
        o.list_all = TRUE;
        o.trim_whitespaces = TRUE;
        int r = w.list(&head, &o);
        if (r != 0 && r != WDI_ERROR_NO_DEVICE) throw std::runtime_error(w.error(r));
    }
    ~DeviceList() {
        if (head) w.free_list(head);
    }
};
std::string sha256(const fs::path &p) {
    BCRYPT_ALG_HANDLE a = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::vector<unsigned char> hash(32), object;
    DWORD n = 0, size = 0;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA256 provider failed");
    BCryptGetProperty(a, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof size, &n, 0);
    object.resize(size);
    if (BCryptCreateHash(a, &h, object.data(), size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(a, 0);
        throw std::runtime_error("SHA256 initialization failed");
    }
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b(65536);
    bool ok = static_cast<bool>(f);
    while (ok && f) {
        f.read(b.data(), b.size());
        auto got = f.gcount();
        if (got && BCryptHashData(h, reinterpret_cast<PUCHAR>(b.data()), static_cast<ULONG>(got), 0) < 0) ok = false;
    }
    if (f.bad()) ok = false;
    if (BCryptFinishHash(h, hash.data(), hash.size(), 0) < 0) ok = false;
    BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    if (!ok) throw std::runtime_error("Cannot hash bundled kernel driver");
    std::ostringstream s;
    s << std::hex << std::setfill('0');
    for (auto c: hash) s << std::setw(2) << unsigned(c);
    return s.str();
}
void verify_kernel(const fs::path &p) {
    if (sha256(p) != kernel_sha256)
        throw std::runtime_error("Bundled libusbK.sys SHA256 does not match official 3.1.0.0 release");
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = p.c_str();
    WINTRUST_DATA d{};
    d.cbStruct = sizeof d;
    d.dwUIChoice = WTD_UI_NONE;
    d.fdwRevocationChecks = WTD_REVOKE_NONE;
    d.dwUnionChoice = WTD_CHOICE_FILE;
    d.pFile = &fi;
    d.dwStateAction = WTD_STATEACTION_VERIFY;
    d.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust(nullptr, &action, &d);
    d.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &d);
    if (r != ERROR_SUCCESS) {
        std::ostringstream s;
        s << "Windows rejected the bundled kernel signature (0x" << std::hex << static_cast<unsigned long>(r)
          << "). No driver installation attempted.";
        throw std::runtime_error(s.str());
    }
    log("Official libusbK.sys SHA256 and Windows Authenticode signature verified.");
}
fs::path secure_workdir() {
    auto base = fs::path(known_folder(FOLDERID_ProgramData)) / L"OmniCoreHost-driver";
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) throw std::runtime_error("Cannot allocate driver work directory");
    wchar_t guid[40]{};
    StringFromGUID2(g, guid, 40);
    // A fresh directory under ProgramData, with no write access granted to ordinary users.
    // Use the GUID at the top level so a pre-existing user-writable parent cannot redirect elevated writes.
    auto p = base;
    p += guid;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1,
                                                              &sd, nullptr))
        throw std::runtime_error("Cannot protect driver work directory");
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    BOOL ok = CreateDirectoryW(p.c_str(), &sa);
    LocalFree(sd);
    if (!ok) throw std::runtime_error("Cannot create protected driver work directory");
    return p;
}
int relaunch_admin() {
    auto exe = executable();
    SHELLEXECUTEINFOW s{};
    s.cbSize = sizeof s;
    s.fMask = SEE_MASK_NOCLOSEPROCESS;
    s.lpVerb = L"runas";
    s.lpFile = exe.c_str();
    s.lpParameters = L"install";
    s.nShow = SW_HIDE;
    log("Windows administrator approval is required to install the OmniCoreSDR driver.");
    if (!ShellExecuteExW(&s)) {
        log("Elevation cancelled or failed (Windows error " + std::to_string(GetLastError()) + ").");
        return 5;
    }
    WaitForSingleObject(s.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(s.hProcess, &code);
    CloseHandle(s.hProcess);
    log("Driver helper exit code: " + std::to_string(code) +
        ". Logs: %ProgramData%\\OmniCoreHost-driver{GUID}\\install.log");
    return static_cast<int>(code);
}
int run(int argc, wchar_t **argv) {
    if (argc < 2 || std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"help") {
        log("OmniCore Host driver assistant (Windows 10/11 x64)\n  och-driver check          Inspect USB 1A86:5537; makes no changes\n  och-driver install        Install libusbK for this SDR (UAC if needed)\n  och-driver extract DIR    Extract unsigned inspection files only; no installation\nExit codes: 0 ready/success, 2 not connected, 3 needs libusbK, 4 error, 5 UAC cancelled.");
        return 0;
    }
    std::wstring cmd = argv[1];
    if ((cmd == L"extract" && argc != 3) || (cmd != L"extract" && argc != 2) ||
        (cmd != L"extract" && cmd != L"check" && cmd != L"install"))
        throw std::runtime_error("Unknown arguments; use och-driver help");
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64)
        throw std::runtime_error("Only native Windows x64 is supported");
    Wdi w;
    if (cmd == L"extract") {
        fs::path p = fs::absolute(argv[2]);
        if (fs::exists(p)) throw std::runtime_error("Extraction directory must not already exist");
        fs::create_directories(p);
        wdi_device_info d{};
        d.vid = vid;
        d.pid = pid;
        d.desc = const_cast<char *>("OmniCoreSDR");
        wdi_options_prepare_driver o{};
        o.driver_type = WDI_LIBUSBK;
        o.vendor_name = const_cast<char *>("OmniCoreSDR");
        o.disable_cat = TRUE;
        o.disable_signing = TRUE;
        auto path = utf8(p.wstring());
        int r = w.prepare(&d, path.c_str(), "omnicore_sdr.inf", &o);
        if (r) throw std::runtime_error(w.error(r));
        verify_kernel(p / L"amd64" / L"libusbK.sys");
        log("Inspection files extracted. The INF is deliberately unsigned and has NOT been installed. Use och-driver install for normal signed installation.");
        return 0;
    }
    DeviceList dl(w);
    unsigned found = 0, need = 0;
    for (auto *d = dl.head; d; d = d->next)
        if (target(d)) {
            ++found;
            if (!ready(d)) ++need;
            log(std::string("OmniCoreSDR ") + (d->device_id ? d->device_id : "") +
                " driver=" + (d->driver ? d->driver : "(none)") + " version=" + version(d->driver_version));
        }
    if (!found) {
        log("OmniCoreSDR is not connected. Software installation can finish now. Connect its USB3 data port, then run och-driver install.");
        return 2;
    }
    if (!need) {
        log("libusbK is already bound. No driver or certificate changes were made.");
        return 0;
    }
    if (cmd == L"check") {
        log("libusbK is required. Run och-driver install after stopping SDR applications.");
        return 3;
    }
    if (!elevated()) return relaunch_admin();
    auto work = secure_workdir();
    auto log_path = work / L"install.log";
    log("Driver preparation and installation log: " + utf8(log_path.wstring()));
    // Capture libwdi's C stdout/stderr diagnostics as well as our own messages.
    if (!_wfreopen(log_path.c_str(), L"a", stdout) || !_wfreopen(log_path.c_str(), L"a", stderr))
        throw std::runtime_error("Cannot open driver install log");
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    // libwdi signs a device-specific catalog using its normal one-time certificate process.
    // No testsigning, signature enforcement, Secure Boot or memory-integrity setting is changed.
    // Generate one package for this non-composite hardware ID; Windows binds matching SDRs only.
    wdi_device_info dev{};
    dev.vid = vid;
    dev.pid = pid;
    dev.desc = const_cast<char *>("OmniCoreSDR");
    for (auto *d = dl.head; d; d = d->next)
        if (target(d) && !ready(d)) {
            dev.device_id = d->device_id;
            dev.hardware_id = d->hardware_id;
            break;
        }
    wdi_options_prepare_driver prep{};
    prep.driver_type = WDI_LIBUSBK;
    prep.vendor_name = const_cast<char *>("OmniCoreSDR");
    prep.disable_cat = TRUE;
    prep.disable_signing = TRUE;
    auto path = utf8(work.wstring());
    int r = w.prepare(&dev, path.c_str(), "omnicore_sdr.inf", &prep);
    if (r) throw std::runtime_error(w.error(r));
    verify_kernel(work / L"amd64" / L"libusbK.sys");
    prep.disable_cat = FALSE;
    prep.disable_signing = FALSE;
    r = w.prepare(&dev, path.c_str(), "omnicore_sdr.inf", &prep);
    if (r) throw std::runtime_error(w.error(r));
    if (!fs::exists(work / L"omnicore_sdr.cat"))
        throw std::runtime_error("Signed catalog missing; refusing installation");
    wdi_options_install_driver options{};
    options.pending_install_timeout = 120000;
    r = w.install(&dev, path.c_str(), "omnicore_sdr.inf", &options);
    if (r) throw std::runtime_error(std::string("Driver install failed: ") + w.error(r));
    bool confirmed = false;
    // Driver replacement temporarily removes/re-enumerates the USB device.
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        Sleep(250);
        DeviceList after(w);
        bool still_need = false;
        unsigned connected = 0;
        for (auto *d = after.head; d; d = d->next)
            if (target(d)) {
                ++connected;
                if (!ready(d)) still_need = true;
            }
        if (connected && !still_need) {
            confirmed = true;
            break;
        }
    }
    if (!confirmed) {
        log("Driver installation returned success but device binding is not yet confirmed. Reconnect the USB3 data cable, then run och-driver check.");
        return 4;
    }
    log("libusbK is installed and bound to OmniCoreSDR. Run och find.");
    return 0;
}
int wmain(int argc, wchar_t **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &e) {
        log(std::string("Error: ") + e.what());
        return 4;
    }
}
