// Shared by the offline tests that run Device / Stream on a FakeLink.
#pragma once
#include "core/internal.hpp"
#include "transport/fake/fake_link.hpp"
#include <cstdio>

namespace och::test {
    using namespace och::core;
    using och::transport::FakeLink;

    inline int failures = 0;

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                         \
            ++och::test::failures;                                                                                     \
        }                                                                                                              \
    } while (0)

    // Runs f and returns the Failure code it throws, OCH_OK if it does not throw.
    template<class F>
    int code_of(F f, std::string *what = nullptr) {
        try {
            f();
        } catch (const Failure &e) {
            if (what) *what = e.what();
            return e.code;
        }
        return OCH_OK;
    }

    // A Device on a new FakeLink, handshake done. The link lives as long as the device.
    struct Bench {
        och_device device;
        FakeLink *link;

        explicit Bench(const std::string &system_info = {}) {
            auto fake = std::make_unique<FakeLink>();
            if (!system_info.empty()) fake->system_info = system_info;
            link = fake.get();
            device.attach(std::move(fake));
        }

        // Events queued so far, oldest first.
        std::vector<och_event> events() {
            std::vector<och_event> out;
            och_event e;
            while (device.events.pop(e, 0)) out.push_back(e);
            return out;
        }

        // Waits for an event of this type; false after one second.
        bool wait_event(uint32_t type, och_event *found = nullptr) {
            const auto deadline = now_ns() + 1000000000ULL;
            och_event e;
            while (now_ns() < deadline)
                if (device.events.pop(e, 20) && e.type == type) {
                    if (found) *found = e;
                    return true;
                }
            return false;
        }
    };

    inline bool has(const std::vector<std::string> &all, const std::string &one) {
        return std::find(all.begin(), all.end(), one) != all.end();
    }
} // namespace och::test
