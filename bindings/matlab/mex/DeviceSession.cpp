#include "DeviceSession.hpp"
#include "MxConvert.hpp"
#include <chrono>
#include <cmath>
#include <limits>

using namespace omnicore_mex;

namespace {
    const double no_value = std::numeric_limits<double>::quiet_NaN();

    // och_signal_source names for the LO inputs.
    uint32_t lo_source(const std::string &name, const std::string &what) {
        if (name == "internal") return OCH_SOURCE_INTERNAL;
        if (name == "external") return OCH_SOURCE_EXTERNAL;
        throw invalid_argument(what + " must be 'internal' or 'external'");
    }
} // namespace

DeviceSession::DeviceSession(const std::string &serial, const ConfigRequest &request, Warnings &warnings) {
    check(och_open(serial.empty() ? nullptr : serial.c_str(), &dev_), "open");
    try {
        info_ = och_struct<och_device_info>();
        check(och_get_info(dev_, &info_), "get_info");
        serial_ = info_.serial;
        // A fresh handle has no applied configuration; start from the library defaults.
        och_config defaults = och_struct<och_config>();
        och_default_config(&defaults);
        och_config next = apply(defaults, request, warnings);
        check(och_configure(dev_, &next), "configure");
        check(och_get_config(dev_, &next), "get_config");
        config_ = next;
        event_thread_ = std::thread(&DeviceSession::event_loop, this);
    } catch (...) {
        och_close(dev_);
        throw;
    }
}

DeviceSession::~DeviceSession() {
    stop_events_ = true;
    if (event_thread_.joinable()) event_thread_.join();
    // Nothing can be reported from here; close errors only mean the device is already gone.
    if (dev_) och_close(dev_);
}

void DeviceSession::event_loop() {
    while (!stop_events_) {
        auto e = och_struct<och_event>();
        const int rc = och_poll_event(dev_, &e, 100);
        if (rc == OCH_OK) {
            tx_events_.push(e);
            std::lock_guard<std::mutex> lock(events_mutex_);
            if (events_.size() >= max_events) events_.pop_front(); // keep the latest
            if (e.type == OCH_EVENT_TRANSPORT) {
                transport_errors_ += e.count ? e.count : 1;
                last_transport_error_ = e.message;
            }
            events_.push_back(e);
        }
        else if (rc != OCH_TIMEOUT)
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // device gone: keep quiet
    }
}

DeviceTime DeviceSession::time() const {
    auto t = och_struct<och_time_status>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        check(och_get_time(dev_, &t), "get_time");
    }
    DeviceTime d;
    d.now = t.now;
    d.last_pps = t.last_pps;
    d.tick_rate = t.tick_rate ? t.tick_rate : config().sample_rate;
    d.valid = t.valid != 0;
    return d;
}

uint64_t DeviceSession::to_ticks(double seconds) const {
    if (!(seconds >= 0) || !std::isfinite(seconds)) throw invalid_argument("StartTime must be a time in seconds, 0 or more");
    return uint64_t(std::llround(seconds * double(time().tick_rate)));
}

mxArray *DeviceSession::time_struct() const {
    auto t = och_struct<och_time_status>();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        check(och_get_time(dev_, &t), "get_time");
    }
    const uint32_t rate = t.tick_rate ? t.tick_rate : config().sample_rate;
    auto ticks = [](uint64_t v) {
        mxArray *a = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
        *mxGetUint64s(a) = v;
        return a;
    };
    const auto &sources = omnicore_bindings::time_sources();
    return StructBuilder()
            .add("Time", make_double(rate ? double(t.now) / rate : no_value))
            .add("TimeTicks", ticks(t.now))
            .add("TickRate", make_double(rate))
            .add("Valid", make_bool(t.valid != 0))
            .add("Armed", make_bool(t.armed != 0))
            .add("Source", make_string(t.source < sources.size() ? sources[t.source] : "none"))
            .add("LastPps", make_double(rate ? double(t.last_pps) / rate : no_value))
            .add("LastPpsTicks", ticks(t.last_pps))
            .add("PpsCount", make_double(t.pps_count))
            .build();
}

void DeviceSession::set_time(double seconds, bool at_next_pps) {
    const uint64_t ticks = to_ticks(seconds);
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_set_time(dev_, ticks, at_next_pps ? 1 : 0), "set_time");
}

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
} // namespace

void DeviceSession::take_notices(Warnings &warnings) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (!transport_errors_) return;
    warnings.push_back({"omnicore:Transport", serial_ + ": " + std::to_string(transport_errors_) +
                                                      " USB transport error(s), latest: " + last_transport_error_ +
                                                      " (see events)"});
    transport_errors_ = 0;
}

mxArray *DeviceSession::events_struct() {
    std::deque<och_event> events;
    {
        std::lock_guard<std::mutex> lock(events_mutex_);
        events.swap(events_);
    }
    const double rate = config().sample_rate;
    const char *fields[] = {"Type", "Message", "HwTime", "HwTicks", "Count", "HostTime"};
    mxArray *out = mxCreateStructMatrix(events.size(), 1, 6, fields);
    for (size_t i = 0; i < events.size(); ++i) {
        const och_event &e = events[i];
        mxArray *ticks = mxCreateNumericMatrix(1, 1, mxUINT64_CLASS, mxREAL);
        *mxGetUint64s(ticks) = e.hw_time;
        mxSetField(out, i, "Type", make_string(event_name(e.type)));
        mxSetField(out, i, "Message", make_string(e.message));
        mxSetField(out, i, "HwTime", make_double(e.hw_time && rate > 0 ? double(e.hw_time) / rate : no_value));
        mxSetField(out, i, "HwTicks", ticks);
        mxSetField(out, i, "Count", make_double(e.count ? e.count : 1));
        mxSetField(out, i, "HostTime", make_double(double(e.host_monotonic_ns) / 1e9));
    }
    return out;
}

och_config DeviceSession::config() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void DeviceSession::configure(const ConfigRequest &request, Warnings &warnings) {
    if (request.empty()) return;
    // A sample-rate change restarts the hardware time; warn only when it had been set.
    const bool rate_change =
            request.sample_rate && uint32_t(std::llround(*request.sample_rate)) != config().sample_rate;
    const bool time_was_valid = rate_change && time().valid;
    std::lock_guard<std::mutex> lock(mutex_);
    Warnings pending;
    och_config next = apply(config_, request, pending);
    check(och_configure(dev_, &next), "configure");
    check(och_get_config(dev_, &next), "get_config");
    if (time_was_valid && next.sample_rate != config_.sample_rate)
        pending.push_back({"omnicore:TimeInvalidated",
                           "the sample-rate change restarted the hardware time; set it again (setTime)"});
    config_ = next;
    warnings.insert(warnings.end(), pending.begin(), pending.end());
}

mxArray *DeviceSession::info_struct() const {
    return StructBuilder()
            .add("Serial", make_string(info_.serial))
            .add("Name", make_string(info_.name))
            .add("Firmware", make_string(info_.firmware))
            .add("UsbDriver", make_string(info_.driver))
            .add("UsbSpeedMbps", make_double(info_.usb_speed_mbps))
            .add("RxChannelCount", make_double(info_.rx_channels))
            .add("TxChannelCount", make_double(info_.tx_channels))
            .add("HardwareTimestamps", make_bool(info_.hardware_timestamps != 0))
            .add("MaxSampleRate", make_double(info_.max_sample_rate))
            .add("LibraryVersion", make_string(och_version()))
            .build();
}

mxArray *DeviceSession::config_struct() const {
    using namespace omnicore_bindings;
    const och_config c = config();
    const auto &modes = agc_modes();
    return StructBuilder()
            .add("SampleRate", make_double(c.sample_rate))
            .add("RxFrequency", make_double(double(c.rx_frequency)))
            .add("TxFrequency", make_double(double(c.tx_frequency)))
            .add("RxBandwidth", make_double(c.rx_bandwidth))
            .add("TxBandwidth", make_double(c.tx_bandwidth))
            .add("RxGain", make_row({c.rx_gain[0], c.rx_gain[1]}))
            .add("TxGain",
                 make_row({tx_attenuation_to_gain(c.tx_attenuation[0]), tx_attenuation_to_gain(c.tx_attenuation[1])}))
            .add("RxGainMode", make_string(modes.at(c.rx_agc < modes.size() ? c.rx_agc : 0)))
            .add("IQBits", make_double(c.usb_iq_bits == 8 ? 8 : 12))
            .add("RxChannels", make_row(mask_channels(c.rx_mask)))
            .add("TxChannels", make_row(mask_channels(c.tx_mask)))
            .build();
}

mxArray *DeviceSession::status_struct() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto temp = och_struct<och_core_temperature>();
    const bool has_temp = check_supported(och_get_core_temperature(dev_, &temp), "get_core_temperature");
    auto ref = och_struct<och_reference_status>();
    const bool has_ref = check_supported(och_get_reference_status(dev_, &ref), "get_reference_status");
    auto pps = och_struct<och_pps_status>();
    const bool has_pps = check_supported(och_get_pps_status(dev_, &pps), "get_pps_status");
    auto vcxo = och_struct<och_vcxo_status>();
    const bool has_vcxo = check_supported(och_get_vcxo_status(dev_, &vcxo), "get_vcxo_status");
    auto time = och_struct<och_time_status>();
    check(och_get_time(dev_, &time), "get_time");
    auto rssi = och_struct<och_rx_rssi>();
    const bool has_rssi = check_supported(och_get_rx_rssi(dev_, &rssi), "get_rx_rssi");

    return StructBuilder()
            .add("Temperature", make_double(has_temp && temp.valid ? temp.milli_celsius / 1000.0 : no_value))
            .add("RefLocked", make_bool(has_ref && ref.lock_known && ref.locked == 1))
            .add("PpsLocked", make_bool(has_pps && pps.lock_known && pps.locked == 1))
            .add("VcxoLocked", make_bool(has_vcxo && vcxo.locked == 1))
            .add("TimeValid", make_bool(time.valid != 0))
            .add("UsbSpeedMbps", make_double(info_.usb_speed_mbps))
            .add("RxRssi", make_row({has_rssi && rssi.valid[0] ? rssi.rssi_db[0] : no_value,
                                     has_rssi && rssi.valid[1] ? rssi.rssi_db[1] : no_value}))
            .build();
}

mxArray *DeviceSession::get(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name == "RxLOSource" || name == "TxLOSource") {
        auto l = och_struct<och_lo_source_config>();
        if (!check_supported(och_get_lo_source(dev_, &l), "get_lo_source")) return make_string("internal");
        const uint32_t s = name == "RxLOSource" ? l.rx_source : l.tx_source;
        return make_string(s == OCH_SOURCE_EXTERNAL ? "external" : "internal");
    }
    if (name == "ClockSource") {
        auto r = och_struct<och_reference_status>();
        if (!check_supported(och_get_reference_status(dev_, &r), "get_reference_status")) return make_string("internal");
        // An unknown source (after a fault) reads as internal, the state setting internal recovers to.
        return make_string(r.source_known && r.source == OCH_SOURCE_EXTERNAL ? "external" : "internal");
    }
    if (name == "TimeSource") {
        auto t = och_struct<och_time_status>();
        check(och_get_time(dev_, &t), "get_time");
        const auto &sources = omnicore_bindings::time_sources();
        return make_string(t.source < sources.size() ? sources[t.source] : "none");
    }
    if (name == "PpsEnable") {
        auto p = och_struct<och_pps_status>();
        return make_bool(check_supported(och_get_pps_status(dev_, &p), "get_pps_status") && p.enabled);
    }
    if (name == "RssiEnable") {
        auto r = och_struct<och_rx_rssi>();
        return make_bool(check_supported(och_get_rx_rssi(dev_, &r), "get_rx_rssi") && r.enabled);
    }
    if (name == "RgbEnable") {
        auto r = och_struct<och_rgb_status>();
        return make_bool(check_supported(och_get_rgb_status(dev_, &r), "get_rgb_status") && r.enabled);
    }
    if (name == "FanEnable") {
        auto f = och_struct<och_fan_status>();
        return make_bool(check_supported(och_get_fan_status(dev_, &f), "get_fan_status") && f.enabled);
    }
    if (name == "RxDCCorrection" || name == "RxIQCorrection") {
        auto c = och_struct<och_rx_correction_config>();
        if (!check_supported(och_get_rx_correction(dev_, &c), "get_rx_correction")) return make_bool(false);
        return make_bool((name == "RxDCCorrection" ? c.dc_enabled : c.iq_enabled) != 0);
    }
    throw invalid_argument("unknown device setting '" + name + "'");
}

void DeviceSession::set(const std::string &name, const mxArray *value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (name == "RxLOSource")
        check(och_set_rx_lo_source(dev_, lo_source(to_string(value, "RxLOSource"), name)), "set_rx_lo_source");
    else if (name == "TxLOSource")
        check(och_set_tx_lo_source(dev_, lo_source(to_string(value, "TxLOSource"), name)), "set_tx_lo_source");
    else if (name == "ClockSource")
        check(och_set_reference_source(dev_, lo_source(to_string(value, "ClockSource"), name)), "set_reference_source");
    else if (name == "TimeSource") {
        uint32_t index = 0;
        try {
            index = omnicore_bindings::source_index(omnicore_bindings::time_sources(), to_string(value, "TimeSource"),
                                                    "TimeSource");
        } catch (const std::invalid_argument &e) {
            throw invalid_argument(e.what());
        }
        check(och_set_time_source(dev_, index), "set_time_source");
    } else if (name == "PpsEnable")
        check(och_set_pps_enabled(dev_, to_bool(value, "PpsEnable")), "set_pps_enabled");
    else if (name == "RssiEnable")
        check(och_set_rx_rssi_enabled(dev_, to_bool(value, "RssiEnable")), "set_rx_rssi_enabled");
    else if (name == "RgbEnable")
        check(och_set_rgb_enabled(dev_, to_bool(value, "RgbEnable")), "set_rgb_enabled");
    else if (name == "FanEnable")
        check(och_set_fan_enabled(dev_, to_bool(value, "FanEnable")), "set_fan_enabled");
    else if (name == "RxDCCorrection")
        check(och_set_rx_dc_correction(dev_, to_bool(value, "RxDCCorrection")), "set_rx_dc_correction");
    else if (name == "RxIQCorrection")
        check(och_set_rx_iq_correction(dev_, to_bool(value, "RxIQCorrection")), "set_rx_iq_correction");
    else
        throw invalid_argument("unknown device setting '" + name + "'");
}
