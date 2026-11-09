// C entry points: AD9361 FIR, clocks, sample rate, RX correction, LO, reference clock, PPS, VCXO.
#include "common.hpp"
using namespace och::core;
using namespace och::capi;

extern "C" {
int och_get_fir(och_device *d, och_direction dir, och_fir_config *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_fir(dir);
        return OCH_OK;
    });
}

int och_set_fir(och_device *d, const och_fir_config *c) {
    return protect([&] {
        device(d);
        require(c != nullptr, OCH_INVALID, "null FIR config");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_fir(*c);
        return OCH_OK;
    });
}

int och_set_fir_enabled(och_device *d, uint32_t r, uint32_t t) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->enable_fir(r, t);
        return OCH_OK;
    });
}

int och_get_clock(och_device *d, och_clock_config *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_clock();
        return OCH_OK;
    });
}

int och_set_clock(och_device *d, const och_clock_config *c) {
    return protect([&] {
        device(d);
        require(c != nullptr, OCH_INVALID, "null clock config");
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_clock(*c);
        return OCH_OK;
    });
}

int och_set_sample_rate(och_device *d, uint32_t rate) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_sample_rate(rate);
        return OCH_OK;
    });
}

int och_get_rx_correction(och_device *d, och_rx_correction_config *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_rx_correction();
        return OCH_OK;
    });
}

int och_set_rx_dc_correction(och_device *d, uint32_t enabled) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_rx_correction(true, enabled);
        return OCH_OK;
    });
}

int och_set_rx_iq_correction(och_device *d, uint32_t enabled) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_rx_correction(false, enabled);
        return OCH_OK;
    });
}

int och_get_lo_source(och_device *d, och_lo_source_config *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_lo_source();
        return OCH_OK;
    });
}

int och_set_rx_lo_source(och_device *d, uint32_t source) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_lo_source(true, source);
        return OCH_OK;
    });
}

int och_set_tx_lo_source(och_device *d, uint32_t source) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_lo_source(false, source);
        return OCH_OK;
    });
}

int och_get_reference_status(och_device *d, och_reference_status *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_reference_status();
        return OCH_OK;
    });
}

int och_set_reference_source(och_device *d, uint32_t source) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_reference_source(source);
        return OCH_OK;
    });
}

int och_test_reference(och_device *d, och_reference_test_result *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->test_reference();
        return OCH_OK;
    });
}

int och_get_pps_status(och_device *d, och_pps_status *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_pps_status();
        return OCH_OK;
    });
}

int och_set_pps_enabled(och_device *d, uint32_t enabled) {
    return protect([&] {
        device(d);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        d->set_pps_enabled(enabled);
        return OCH_OK;
    });
}

int och_get_vcxo_status(och_device *d, och_vcxo_status *c) {
    return protect([&] {
        device(d);
        out_struct(c);
        std::lock_guard<std::mutex> lock(d->state_mutex);
        *c = d->get_vcxo_status();
        return OCH_OK;
    });
}
}
