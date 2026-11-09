// C entry points: power status, RGB, fan, RX RSSI, core temperature.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

extern "C" {
int och_get_power_status(och_device *d, och_power_status *p, uint32_t timeout) {
    return protect([&] {
        device(d);
        out_struct(p);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *p = d->get_power_status(timeout);
        return OCH_OK;
    });
}

int och_get_rgb_status(och_device *d, och_rgb_status *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_rgb_status();
        return OCH_OK;
    });
}

int och_get_fan_status(och_device *d, och_fan_status *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_fan_status();
        return OCH_OK;
    });
}

int och_get_rx_rssi(och_device *d, och_rx_rssi *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_rx_rssi();
        return OCH_OK;
    });
}

int och_set_rx_rssi_enabled(och_device *d, uint32_t enabled) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_rx_rssi_enabled(enabled);
        return OCH_OK;
    });
}

int och_get_core_temperature(och_device *d, och_core_temperature *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_core_temperature();
        return OCH_OK;
    });
}

int och_set_rgb_enabled(och_device *d, uint32_t enabled) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_rgb_enabled(enabled);
        return OCH_OK;
    });
}

int och_set_fan_enabled(och_device *d, uint32_t enabled) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_fan_enabled(enabled);
        return OCH_OK;
    });
}
}
