#include "internal.hpp"
#include <algorithm>
using namespace och::core;
using namespace och::transport;

namespace {
    // One bounded bulk transfer on a diagnostics endpoint; 0 bytes on timeout.
    ULONG diag_transfer(Usb &usb, UCHAR ep, uint8_t *data, ULONG size, ULONG timeout_ms) {
        usb.timeout(ep, std::max<ULONG>(1, timeout_ms));
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(event.h != nullptr, OCH_IO, "power status event allocation failed");
        OVERLAPPED ov{};
        ov.hEvent = event.h;
        ULONG count = 0;
        BOOL ok = (ep & 0x80) ? usb.api.read(usb.h, ep, data, size, &count, &ov)
                              : usb.api.write(usb.h, ep, data, size, &count, &ov);
        const auto submit_error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && submit_error == ERROR_SEM_TIMEOUT) return 0;
        if (!ok && submit_error != ERROR_IO_PENDING)
            throw Failure(OCH_IO, "diagnostics submit: " + utf8(syserr(submit_error)));
        if (!ok && !usb.api.result(usb.h, &ov, &count, TRUE)) {
            const auto code = GetLastError();
            if (code == ERROR_SEM_TIMEOUT) return 0;
            throw Failure(OCH_IO, "diagnostics completion: " + utf8(syserr(code)));
        }
        require(count <= size, OCH_PROTOCOL, "diagnostics transfer length exceeds request");
        return count;
    }
}

och_power_status och_device::get_power_status(uint32_t timeout_ms) {
    require(timeout_ms <= 2000, OCH_INVALID, "power status timeout must be 0..2000 ms");
    const bool present = usb.has(0x83);
    // Bridge 1.0.1+: EP3 OUT takes "typec?" and the reply arrives on EP83.
    const bool query = present && usb.has(0x03);
    // Diagnostics must not introduce USB traffic while a stream exists.
    if (query && timeout_ms && !rx && !tx) {
        usb.policy(0x83, RAW_IO, FALSE);
        usb.policy(0x83, ALLOW_PARTIAL_READS, TRUE);
        usb.policy(0x83, AUTO_FLUSH, FALSE);
        const uint64_t deadline = now_ns() + uint64_t(timeout_ms) * 1000000;
        std::array<uint8_t, 4096> bytes{};
        // 1. Drop log text queued before the query (startup log, old replies).
        for (size_t drained = 0; drained < 65536;) {
            const auto n = diag_transfer(usb, 0x83, bytes.data(), ULONG(bytes.size()), 5);
            if (!n) break;
            drained += n;
            require(now_ns() < deadline, OCH_TIMEOUT, "EP83 log did not drain before the Type-C query");
        }
        // 2. Ask, then parse only what follows the question.
        static const char ask[] = "typec?\n";
        require(diag_transfer(usb, 0x03, (uint8_t *) ask, ULONG(sizeof ask - 1), 200) == sizeof ask - 1, OCH_IO,
                "Type-C query was not accepted on EP 0x03");
        PowerLogParser fresh;
        for (size_t total = 0; total < 16384;) {
            const auto now = now_ns();
            if (now >= deadline) break;
            const auto n =
                    diag_transfer(usb, 0x83, bytes.data(), ULONG(bytes.size()),
                                  ULONG(std::min<uint64_t>(25, std::max<uint64_t>(1, (deadline - now) / 1000000))));
            if (n) {
                fresh.feed(bytes.data(), n);
                total += n;
            }
            const auto s = fresh.status();
            if (s.snapshot_valid || s.conflicting_snapshots) break;
            // "HD3SS3220: ERR ..." invalidates the parser: stop waiting.
            if (n && std::search(bytes.begin(), bytes.begin() + n, "HD3SS3220: ERR",
                                 (const char *) "HD3SS3220: ERR" + 14) != bytes.begin() + n)
                break;
        }
        power_log = fresh;
        auto snapshot = power_log.status();
        snapshot.log_endpoint_present = 1u;
        snapshot.live = snapshot.snapshot_valid ? 1u : 0u;
        return snapshot;
    }
    if (present && timeout_ms && !rx && !tx && !query) {
        usb.policy(0x83, RAW_IO, FALSE);
        usb.policy(0x83, ALLOW_PARTIAL_READS, TRUE);
        usb.policy(0x83, AUTO_FLUSH, FALSE);
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        require(event.h != nullptr, OCH_IO, "power status event allocation failed");
        std::array<uint8_t, 4096> bytes{};
        const uint64_t deadline = now_ns() + uint64_t(timeout_ms) * 1000000;
        size_t total = 0;
        while (total < 16384) {
            const auto now = now_ns();
            if (now >= deadline) break;
            const auto remaining = static_cast<ULONG>(std::max<uint64_t>(1, (deadline - now) / 1000000));
            usb.timeout(0x83, std::min<ULONG>(remaining, 25));
            ResetEvent(event.h);
            OVERLAPPED ov{};
            ov.hEvent = event.h;
            ULONG count = 0;
            const auto requested = static_cast<ULONG>(std::min(bytes.size(), size_t(16384) - total));
            BOOL ok = usb.api.read(usb.h, 0x83, bytes.data(), requested, &count, &ov);
            const auto submit_error = ok ? ERROR_SUCCESS : GetLastError();
            if (!ok && submit_error != ERROR_IO_PENDING && submit_error != ERROR_SEM_TIMEOUT)
                throw Failure(OCH_IO, "power log submit: " + utf8(syserr(submit_error)));
            if (!ok && submit_error == ERROR_IO_PENDING && !usb.api.result(usb.h, &ov, &count, TRUE)) {
                const auto code = GetLastError();
                if (code != ERROR_SEM_TIMEOUT) throw Failure(OCH_IO, "power log completion: " + utf8(syserr(code)));
                count = 0;
            }
            if (!ok && submit_error == ERROR_SEM_TIMEOUT) count = 0;
            require(count <= requested, OCH_PROTOCOL, "power log transfer length exceeds request");
            if (count) {
                power_log.feed(bytes.data(), count);
                total += count;
            } else if (power_log.status().snapshot_valid || power_log.status().conflicting_snapshots)
                break;
        }
    }
    auto snapshot = power_log.status();
    snapshot.log_endpoint_present = present ? 1u : 0u;
    snapshot.live = 0u; // startup log, or this handle's cache
    return snapshot;
}
