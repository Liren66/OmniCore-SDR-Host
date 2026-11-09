// C entry points: multi-board synchronisation.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

extern "C" {
void och_default_sync_config(och_sync_config *c) {
    if (!c) return;
    *c = {};
    c->struct_size = sizeof *c;
    c->trig_width = 80;
}

void och_default_multi_sync_args(och_multi_sync_args *a) {
    if (!a) return;
    *a = {};
    a->struct_size = sizeof *a;
    a->mode = OCH_SYNC_CHAIN;
    a->mcs = 1;
    a->lock_timeout_ms = 15000;
}

int och_get_sync_status(och_device *d, och_sync_status *s) {
    return protect([&] {
        device(d);
        out_struct(s);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *s = read_sync_status(*d);
        return OCH_OK;
    });
}

int och_set_sync(och_device *d, const och_sync_config *c) {
    return protect([&] {
        device(d);
        require(c && c->struct_size == sizeof(*c), OCH_INVALID, "och_sync_config struct_size mismatch");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        apply_sync_config(*d, *c);
        return OCH_OK;
    });
}

int och_sync_trigger(och_device *d, uint32_t mode) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        sync_trigger(*d, mode);
        return OCH_OK;
    });
}

int och_sync_clear(och_device *d) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->require_capability("sync");
        d->command("sync_clear!");
        return OCH_OK;
    });
}

int och_mcs_step(och_device *d, uint32_t step) {
    return protect([&] {
        device(d);
        require(step <= 5, OCH_INVALID, "MCS step must be 0..5");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->require_capability("sync");
        require(!d->rx_on && !d->tx_on, OCH_BUSY, "stop RX/TX streams before MCS");
        d->command("mcs_step=" + std::to_string(step));
        return OCH_OK;
    });
}

int och_multi_sync(och_device *const *devs, size_t n, const och_multi_sync_args *a, och_multi_sync_result *r) {
    return protect([&] {
        require(a && a->struct_size == sizeof(*a), OCH_INVALID, "och_multi_sync_args struct_size mismatch");
        if (r)
            for (size_t i = 0; i < n; ++i)
                require(r[i].struct_size == sizeof(*r), OCH_INVALID, "och_multi_sync_result struct_size mismatch");
        multi_sync(devs, n, *a, r);
        return OCH_OK;
    });
}
}
