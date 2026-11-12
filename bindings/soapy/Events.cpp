// Device event thread: drains och_poll_event and logs each event (SOAPY_PLAN.md 4.5).
#include "OmniCoreDevice.hpp"
#include <SoapySDR/Logger.hpp>
#include <chrono>

using namespace omnicore_soapy;

namespace {
    const char *event_name(uint32_t type) {
        switch (type) {
        case OCH_EVENT_DEVICE: return "device";
        case OCH_EVENT_RX_SYNC: return "rx_sync";
        case OCH_EVENT_HOST_OVERFLOW: return "host_overflow";
        case OCH_EVENT_TRANSPORT: return "transport";
        case OCH_EVENT_TX_STARVED: return "tx_starved";
        case OCH_EVENT_RECONFIGURED: return "reconfigured";
        case OCH_EVENT_TX_BURST_ACK: return "tx_burst_ack";
        case OCH_EVENT_TX_LATE: return "tx_late";
        case OCH_EVENT_TX_UNDERFLOW: return "tx_underflow";
        case OCH_EVENT_TX_ERROR: return "tx_error";
        case OCH_EVENT_RX_GAP: return "rx_gap";
        case OCH_EVENT_RX_LATE: return "rx_late";
        case OCH_EVENT_TIME_INVALIDATED: return "time_invalidated";
        case OCH_EVENT_SYNC_IN: return "sync_in";
        default: return "unknown";
        }
    }

    SoapySDRLogLevel event_level(uint32_t type) {
        switch (type) {
        case OCH_EVENT_TRANSPORT:
        case OCH_EVENT_TX_ERROR:
            return SOAPY_SDR_ERROR;
        case OCH_EVENT_DEVICE:
        case OCH_EVENT_TX_LATE:
        case OCH_EVENT_TX_UNDERFLOW:
        case OCH_EVENT_RX_LATE:
        case OCH_EVENT_TIME_INVALIDATED:
            return SOAPY_SDR_WARNING;
        default:
            // RX gaps and overflows already reach the application through readStream. TX_STARVED
            // also fires in the normal idle time between bursts; readStreamStatus reports the
            // ones inside a burst as underflows.
            return SOAPY_SDR_DEBUG;
        }
    }
    bool is_tx_event(uint32_t type) {
        return type == OCH_EVENT_TX_STARVED || type == OCH_EVENT_TX_BURST_ACK || type == OCH_EVENT_TX_LATE ||
               type == OCH_EVENT_TX_UNDERFLOW || type == OCH_EVENT_TX_ERROR;
    }
    constexpr size_t max_tx_events = 1024;
} // namespace

void OmniCoreDevice::clear_tx_events() {
    std::lock_guard<std::mutex> lock(tx_events_mutex_);
    tx_events_.clear();
    tx_events_dropped_ = 0;
}

void OmniCoreDevice::event_loop() {
    bool reported_failure = false;
    while (!stop_events_) {
        auto e = och_struct<och_event>();
        const int rc = och_poll_event(dev_, &e, 100);
        if (rc == OCH_TIMEOUT) continue;
        if (rc != OCH_OK) {
            if (!reported_failure)
                SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore: event polling failed: %s", och_last_error());
            reported_failure = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        reported_failure = false;
        if (tx_listening_ && is_tx_event(e.type)) {
            {
                std::lock_guard<std::mutex> lock(tx_events_mutex_);
                if (tx_events_.size() >= max_tx_events) {
                    tx_events_.pop_front();
                    ++tx_events_dropped_;
                }
                tx_events_.push_back(e);
            }
            tx_events_cv_.notify_all();
        }
        if (e.count > 1)
            SoapySDR::logf(event_level(e.type), "OmniCore %s x%u: %s", event_name(e.type), e.count, e.message);
        else
            SoapySDR::logf(event_level(e.type), "OmniCore %s: %s", event_name(e.type), e.message);
    }
}
