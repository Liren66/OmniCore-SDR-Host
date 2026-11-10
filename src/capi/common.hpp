// Shared helpers for the C entry points in src/capi/.
#pragma once
#include "../core/internal.hpp"
#include <cstddef>
#include <limits>
#include <string>

namespace och::capi {
    using namespace och::core;

    // Text for och_last_error(), one per thread and shared by every capi file. A function-local
    // thread_local rather than an inline thread_local variable: Apple's linker rejects the latter
    // for std::string (duplicate thread-local initialization routine).
    inline std::string &last_error() {
        thread_local std::string text;
        return text;
    }

    template<class F>
    int protect(F f) noexcept {
        try {
            return f();
        } catch (const Failure &e) {
            last_error() = e.what();
            return e.code;
        } catch (const std::exception &e) {
            last_error() = e.what();
            return OCH_IO;
        } catch (...) {
            last_error() = "unexpected internal error";
            return OCH_IO;
        }
    }

    template<class T>
    void out_struct(T *p) {
        require(p && p->struct_size == sizeof(T), OCH_INVALID, "output struct_size mismatch");
    }

    // Structures extended in 0.6.0 / 0.6.1 accept the earlier, shorter layouts too.
    constexpr size_t meta_v1 = offsetof(och_metadata, time_ticks), event_v1 = offsetof(och_event, hw_time),
                     event_v2 = offsetof(och_event, count);

    template<class T>
    void out_struct_v(T *p, size_t v1, size_t v2 = 0) {
        require(p && (p->struct_size == sizeof(T) || p->struct_size == v1 || (v2 && p->struct_size == v2)), OCH_INVALID,
                "output struct_size mismatch");
    }

    inline void device(och_device *p) { require(p != nullptr, OCH_INVALID, "null device"); }
} // namespace och::capi
