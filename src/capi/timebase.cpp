// C entry points: hardware time base.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

extern "C" {
int och_get_time(och_device *d, och_time_status *t) {
    return protect([&] {
        device(d);
        out_struct(t);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *t = d->get_time();
        return OCH_OK;
    });
}

int och_set_time(och_device *d, uint64_t ticks, uint32_t at_next_pps) {
    return protect([&] {
        device(d);
        require(at_next_pps <= 1, OCH_INVALID, "at_next_pps must be 0 or 1");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->require_capability("hw_time");
        d->command((at_next_pps ? "time_next_pps=" : "time_now=") + std::to_string(ticks));
        return OCH_OK;
    });
}

int och_set_time_source(och_device *d, uint32_t source) {
    return protect([&] {
        device(d);
        require(source <= OCH_TIME_SOURCE_SYNC, OCH_INVALID, "time source must be 0..3");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->require_capability("hw_time");
        if (source == OCH_TIME_SOURCE_SYNC) d->require_capability("sync");
        static const char *names[] = {"none", "external", "internal", "sync"};
        d->command(std::string("time_source=") + names[source]);
        return OCH_OK;
    });
}
}
