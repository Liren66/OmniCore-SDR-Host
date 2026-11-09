// C entry points: version, last error, device defaults, enumeration, open/close, configuration, raw commands, events.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

extern "C" {
const char *och_version() { return OCH_VERSION; }
const char *och_last_error() { return last_error().c_str(); }

void och_default_config(och_config *c) {
    if (!c) return;
    *c = {};
    c->struct_size = sizeof(*c);
    c->sample_rate = 30720000;
    c->rx_mask = 3;
    c->tx_mask = 0;
    c->rx_frequency = c->tx_frequency = 1000000000ULL;
    c->rx_bandwidth = c->tx_bandwidth = 18000000;
    c->rx_gain[0] = c->rx_gain[1] = 30;
    c->tx_attenuation[0] = c->tx_attenuation[1] = 10;
    c->rx_agc = 2;
}

int och_list(och_device_info *output, size_t capacity, size_t *count) {
    return protect([&] {
        require(count && (!capacity || output), OCH_INVALID, "invalid device list output");
        auto all = och::transport::scanDevices(0x1a86, 0x5537);
        *count = all.size();
        for (size_t i = 0; i < std::min(capacity, all.size()); ++i) out_struct(&output[i]);
        for (size_t i = 0; i < std::min(capacity, all.size()); ++i) fill_info(output[i], all[i]);
        return OCH_OK;
    });
}

int och_open(const char *serial, och_device **out) {
    return protect([&] {
        require(out != nullptr, OCH_INVALID, "null device output");
        *out = nullptr;
        auto all = och::transport::scanDevices(0x1a86, 0x5537);
        std::vector<och::transport::DeviceInfo> match;
        for (auto &d: all)
            if (!serial || !*serial || serial_of(d) == serial) match.push_back(d);
#ifdef _WIN32
        require(!match.empty(), OCH_NOT_FOUND, "OmniCoreSDR with libusbK not found; run och-driver check");
#else
        require(!match.empty(), OCH_NOT_FOUND, "OmniCoreSDR USB device not found (VID 1a86, PID 5537)");
#endif
        require(match.size() == 1, OCH_INVALID, "multiple devices; specify a serial number");
        auto d = std::make_unique<och_device>();
        d->open(match.front());
        *out = d.release();
        return OCH_OK;
    });
}

int och_close(och_device *d) {
    return protect([&] {
        device(d);
        {
            std::lock_guard<std::mutex> lock(d->state_mutex);
            require(!d->rx && !d->tx, OCH_BUSY, "destroy RX/TX streams before closing device");
        }
        delete d;
        return OCH_OK;
    });
}

int och_get_info(och_device *d, och_device_info *i) {
    return protect([&] {
        device(d);
        out_struct(i);
        *i = d->info;
        return OCH_OK;
    });
}

int och_configure(och_device *d, const och_config *c) {
    return protect([&] {
        device(d);
        require(c != nullptr, OCH_INVALID, "null config");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->configure(*c);
        return OCH_OK;
    });
}

int och_get_config(och_device *d, och_config *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        require(d->configured, OCH_INVALID, "device has not been configured by this handle");
        *c = d->config;
        return OCH_OK;
    });
}

int och_command(och_device *d, const char *cmd, char *reply, size_t capacity, uint32_t timeout) {
    return protect([&] {
        device(d);
        require(cmd && reply && capacity, OCH_INVALID, "invalid command/reply buffer");
        reply[0] = 0;
        std::lock_guard<std::mutex> lock(d->state_mutex);
        std::string text(cmd);
        bool query = !text.empty() && text.back() == '?' && text.find('=') == std::string::npos;
        require(query || (!d->rx && !d->tx), OCH_BUSY, "raw mutations require no streams; use device configure");
        if (!query) {
            d->configured = false;
            d->lo_sources = -1;
        } // A failed firmware command may have applied partially.
        auto result = d->command(text, timeout);
        copy_text(reply, capacity, result);
        require(result.size() < capacity, OCH_BUFFER_SMALL, "command reply buffer too small (reply truncated)");
        return OCH_OK;
    });
}

int och_poll_event(och_device *d, och_event *e, uint32_t timeout) {
    return protect([&] {
        device(d);
        out_struct_v(e, event_v1, event_v2);
        std::unique_lock<std::mutex> lock(d->events_mutex);
        if (!d->events_cv.wait_for(lock, std::chrono::milliseconds(timeout), [&] { return !d->events.empty(); }))
            return OCH_TIMEOUT;
        const auto n = e->struct_size;
        memcpy(e, &d->events.front(), n);
        e->struct_size = n;
        d->events.pop_front();
        return OCH_OK;
    });
}
}
