// Loads the OmniCore Host library before the first och_* call (MEX_PLAN.md 6.3).
//
// Windows does not look for a MEX file's dependencies in the MEX file's own folder. The MSVC
// build links omnicore.dll with /DELAYLOAD, and this preloads it by full path from next to the
// MEX file (libusbK.dll beside it is then found too), falling back to the normal search (PATH,
// where the installer puts the OmniCore Host bin folder). Once a module named omnicore.dll is
// loaded, the delay-load helper binds to it. Elsewhere rpath already handles this: no-op.
#include "LibraryLoader.hpp"
#include "Error.hpp"

#ifdef _WIN32
#include <string>
#include <windows.h>

namespace {
    std::wstring mex_folder() {
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&mex_folder), &self))
            return L"";
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(self, &path[0], DWORD(path.size()));
            if (n == 0) return L"";
            if (n < path.size()) {
                path.resize(n);
                break;
            }
            path.resize(path.size() * 2);
        }
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? L"" : path.substr(0, slash);
    }

    std::string narrow(const std::wstring &w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }
} // namespace

void omnicore_mex::ensure_library_loaded() {
    static bool loaded = false;
    if (loaded || GetModuleHandleW(L"omnicore.dll")) {
        loaded = true;
        return;
    }
    const std::wstring local = mex_folder() + L"\\omnicore.dll";
    if (LoadLibraryExW(local.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) || LoadLibraryW(L"omnicore.dll")) {
        loaded = true;
        return;
    }
    throw Error(
            "omnicore:LibraryNotFound",
            "cannot load omnicore.dll (looked in " + narrow(local) +
                    " and on PATH). Install OmniCore Host or copy omnicore.dll and libusbK.dll next to the MEX file");
}
#else
void omnicore_mex::ensure_library_loaded() {}
#endif
