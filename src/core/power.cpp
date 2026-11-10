#include "internal.hpp"
#include <algorithm>
using namespace och::core;
using namespace och::transport;

namespace {
    // One bounded bulk transfer on a diagnostics endpoint; 0 bytes on timeout.
    size_t diag_transfer(Link &link, Pipe pipe, uint8_t *data, size_t size, uint32_t timeout_ms) {
        const auto count = bounded_transfer(link, pipe, data, size, timeout_ms, "diagnostics");
        require(count <= size, OCH_PROTOCOL, "diagnostics transfer length exceeds request");
        return count;
    }
}

och_power_status Device::get_power_status(uint32_t timeout_ms) {
    require(timeout_ms <= 2000, OCH_INVALID, "power status timeout must be 0..2000 ms");
    const bool present = link->has(Pipe::diag_in);
    // Bridge 1.0.1+: EP3 OUT takes "typec?" and the reply arrives on EP83.
    const bool query = present && link->has(Pipe::diag_out);
    // Diagnostics must not introduce USB traffic while a stream exists.
    if (query && timeout_ms && !rx && !tx) {
        const uint64_t deadline = now_ns() + uint64_t(timeout_ms) * 1000000;
        std::array<uint8_t, 4096> bytes{};
        // 1. Drop log text queued before the query (startup log, old replies).
        for (size_t drained = 0; drained < 65536;) {
            const auto n = diag_transfer(*link, Pipe::diag_in, bytes.data(), bytes.size(), 5);
            if (!n) break;
            drained += n;
            require(now_ns() < deadline, OCH_TIMEOUT, "EP83 log did not drain before the Type-C query");
        }
        // 2. Ask, then parse only what follows the question.
        static const char ask[] = "typec?\n";
        require(diag_transfer(*link, Pipe::diag_out, (uint8_t *) ask, sizeof ask - 1, 200) == sizeof ask - 1, OCH_IO,
                "Type-C query was not accepted on EP 0x03");
        PowerLogParser fresh;
        for (size_t total = 0; total < 16384;) {
            const auto now = now_ns();
            if (now >= deadline) break;
            const auto n =
                    diag_transfer(*link, Pipe::diag_in, bytes.data(), bytes.size(),
                                  uint32_t(std::min<uint64_t>(25, std::max<uint64_t>(1, (deadline - now) / 1000000))));
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
        std::array<uint8_t, 4096> bytes{};
        const uint64_t deadline = now_ns() + uint64_t(timeout_ms) * 1000000;
        size_t total = 0;
        while (total < 16384) {
            const auto now = now_ns();
            if (now >= deadline) break;
            const auto remaining = static_cast<uint32_t>(std::max<uint64_t>(1, (deadline - now) / 1000000));
            const auto requested = std::min(bytes.size(), size_t(16384) - total);
            const auto count = bounded_transfer(*link, Pipe::diag_in, bytes.data(), requested,
                                                std::min<uint32_t>(remaining, 25), "power log");
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
