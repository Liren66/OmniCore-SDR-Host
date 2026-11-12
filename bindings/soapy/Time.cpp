// Reference clock, PPS time source and the 64-bit hardware sample time (1 tick = 1 sample).
#include "common/Mapping.hpp"
#include "OmniCoreDevice.hpp"
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <cctype>

using namespace omnicore_bindings;
using namespace omnicore_soapy;

namespace {
    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }
    // "" or "now": the free-running counter; "pps": its value latched at the last PPS edge.
    bool is_pps(const std::string &what) {
        const auto w = lower(what);
        if (w.empty() || w == "now") return false;
        if (w == "pps") return true;
        throw std::invalid_argument("hardware time '" + what + "' is not supported; use \"\" or \"pps\"");
    }
} // namespace

std::vector<std::string> OmniCoreDevice::listClockSources() const { return clock_sources(); }

void OmniCoreDevice::setClockSource(const std::string &source) {
    const uint32_t index = source_index(clock_sources(), source, "clock source");
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_set_reference_source(dev_, index), "set_reference_source");
}

std::string OmniCoreDevice::getClockSource() const {
    auto s = och_struct<och_reference_status>();
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_get_reference_status(dev_, &s), "get_reference_status");
    // An unknown source (after a fault) reads as internal, the state set_clock_source recovers to.
    return s.source_known && s.source < clock_sources().size() ? clock_sources()[s.source] : clock_sources()[0];
}

std::vector<std::string> OmniCoreDevice::listTimeSources() const { return time_sources(); }

void OmniCoreDevice::setTimeSource(const std::string &source) {
    const uint32_t index = source_index(time_sources(), source, "time source");
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_set_time_source(dev_, index), "set_time_source");
}

std::string OmniCoreDevice::getTimeSource() const {
    auto t = och_struct<och_time_status>();
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_get_time(dev_, &t), "get_time");
    return t.source < time_sources().size() ? time_sources()[t.source] : time_sources()[0];
}

bool OmniCoreDevice::hasHardwareTime(const std::string &what) const {
    // gr-soapy asks once, with "", and only then tags rx_time.
    try {
        is_pps(what);
    } catch (const std::invalid_argument &) {
        return false;
    }
    return info_.hardware_timestamps != 0;
}

double OmniCoreDevice::tick_rate() const {
    auto t = och_struct<och_time_status>();
    check(och_get_time(dev_, &t), "get_time");
    return t.tick_rate ? double(t.tick_rate) : double(config_.sample_rate);
}

long long OmniCoreDevice::getHardwareTime(const std::string &what) const {
    const bool pps = is_pps(what);
    auto t = och_struct<och_time_status>();
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_get_time(dev_, &t), "get_time");
    const double rate = t.tick_rate ? double(t.tick_rate) : double(config_.sample_rate);
    return SoapySDR::ticksToTimeNs((long long)(pps ? t.last_pps : t.now), rate);
}

void OmniCoreDevice::setHardwareTime(long long timeNs, const std::string &what) {
    const bool pps = is_pps(what);
    if (timeNs < 0) throw std::invalid_argument("hardware time must not be negative");
    std::lock_guard<std::mutex> lock(mutex_);
    const long long ticks = SoapySDR::timeNsToTicks(timeNs, tick_rate());
    check(och_set_time(dev_, uint64_t(ticks), pps ? 1 : 0), "set_time");
}
