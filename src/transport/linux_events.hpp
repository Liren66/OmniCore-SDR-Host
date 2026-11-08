// Internal compatibility surface for the shared stream worker and fake transport.
// No Windows ABI is exposed by the public SDK.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#include <string>
#include <new>

using BOOL = int;
using UCHAR = unsigned char;
using ULONG = uint32_t;
using DWORD = uint32_t;
using PUCHAR = UCHAR *;
using PULONG = ULONG *;
using PVOID = void *;
#define WINAPI
constexpr BOOL TRUE = 1, FALSE = 0;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_IO_PENDING = 997, ERROR_IO_INCOMPLETE = 996, ERROR_OPERATION_ABORTED = 995,
                ERROR_NOT_FOUND = 1168, ERROR_SEM_TIMEOUT = 121, ERROR_ALREADY_EXISTS = 183, WAIT_OBJECT_0 = 0,
                WAIT_TIMEOUT = 258, WAIT_FAILED = 0xffffffff, INFINITE = 0xffffffff;
inline thread_local DWORD och_native_error = 0;
inline DWORD GetLastError() { return och_native_error; }
inline void SetLastError(DWORD e) { och_native_error = e; }

struct OchNativeHandle {
    bool signaled = false;
    int fd = -1;
};

using HANDLE = OchNativeHandle *;
inline HANDLE const INVALID_HANDLE_VALUE = nullptr;
inline std::mutex och_event_mutex;
inline std::condition_variable och_event_cv;

inline HANDLE CreateEventW(void *, BOOL, BOOL initial, void *) {
    return new (std::nothrow) OchNativeHandle{initial != 0, -1};
}

inline BOOL CloseHandle(HANDLE h) {
    if (h) {
        if (h->fd >= 0) ::close(h->fd);
        delete h;
    }
    return TRUE;
}

inline BOOL SetEvent(HANDLE h) {
    if (!h) return FALSE;
    {
        std::lock_guard<std::mutex> lock(och_event_mutex);
        h->signaled = true;
    }
    och_event_cv.notify_all();
    return TRUE;
}

inline BOOL ResetEvent(HANDLE h) {
    if (!h) return FALSE;
    std::lock_guard<std::mutex> lock(och_event_mutex);
    h->signaled = false;
    return TRUE;
}

inline DWORD WaitForMultipleObjects(DWORD n, const HANDLE *handles, BOOL all, DWORD timeout) {
    if (!n || all) return WAIT_FAILED;
    for (DWORD i = 0; i < n; ++i)
        if (!handles[i]) return WAIT_FAILED;
    std::unique_lock<std::mutex> lock(och_event_mutex);
    auto ready = [&] {
        for (DWORD i = 0; i < n; ++i)
            if (handles[i]->signaled) return true;
        return false;
    };
    if (timeout == INFINITE)
        och_event_cv.wait(lock, ready);
    else if (!och_event_cv.wait_for(lock, std::chrono::milliseconds(timeout), ready))
        return WAIT_TIMEOUT;
    for (DWORD i = 0; i < n; ++i)
        if (handles[i]->signaled) return WAIT_OBJECT_0 + i;
    return WAIT_FAILED;
}

inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
// flock also releases ownership after a crash. Do not unlink the lock inode:
// waiters and new openers must always contend on the same file.
inline HANDLE CreateMutexW(void *, BOOL, const wchar_t *name) {
    std::string path = "/tmp/";
    for (; *name; ++name)
        path += (*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z') || (*name >= '0' && *name <= '9')
                        ? char(*name)
                        : '_';
    path += ".lock";
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0666);
    if (fd < 0) {
        SetLastError(errno);
        return nullptr;
    }
    int rc = flock(fd, LOCK_EX | LOCK_NB);
    if (rc && errno != EWOULDBLOCK) {
        int e = errno;
        ::close(fd);
        SetLastError(e);
        return nullptr;
    }
    auto *h = new (std::nothrow) OchNativeHandle{false, fd};
    if (!h) {
        ::close(fd);
        return nullptr;
    }
    SetLastError(rc ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
    return h;
}

namespace och::transport {
    struct LinuxTransfer;
}

struct OVERLAPPED {
    HANDLE hEvent = nullptr;
    std::shared_ptr<och::transport::LinuxTransfer> transfer;
};

using LPOVERLAPPED = OVERLAPPED *;
