// Sensors (read-only status) and settings (board switches safe to change while streaming,
// except iq_bits).
#include "Mapping.hpp"
#include "OmniCoreDevice.hpp"
#include <cmath>
#include <cstdio>

using namespace omnicore_soapy;

namespace {
    SoapySDR::ArgInfo arg(const std::string &key, const std::string &name, SoapySDR::ArgInfo::Type type,
                          const std::string &description, const std::string &units = "",
                          const std::vector<std::string> &options = {}) {
        SoapySDR::ArgInfo a;
        a.key = key;
        a.name = name;
        a.type = type;
        a.description = description;
        a.units = units;
        a.options = options;
        return a;
    }
    std::string flag(bool v) { return v ? "true" : "false"; }
    std::string number(double v) {
        char s[32];
        std::snprintf(s, sizeof s, "%.3f", v);
        return s;
    }

    const SoapySDR::ArgInfoList &device_sensors() {
        using T = SoapySDR::ArgInfo;
        static const SoapySDR::ArgInfoList list{
                arg("temperature", "Zynq temperature", T::FLOAT, "Zynq die temperature", "C"),
                arg("ref_locked", "Reference locked", T::BOOL,
                    "PPS frequency monitor within +/-1 ppm; false when unknown (judge the ADF4002 by LED D5)"),
                arg("pps_locked", "PPS locked", T::BOOL, "PPS measured within +/-1 ppm of nominal"),
                arg("vcxo_locked", "VCXO locked", T::BOOL, "CH32 PPS loop tracking within +/-1 us; false outside PPS mode"),
                arg("time_valid", "Time valid", T::BOOL, "hardware sample time is valid (a sample-rate change invalidates it)"),
                arg("usb_speed", "USB speed", T::INT, "USB link speed", "Mbps"),
        };
        return list;
    }
} // namespace

std::vector<std::string> OmniCoreDevice::listSensors() const {
    std::vector<std::string> keys;
    for (auto &a: device_sensors()) keys.push_back(a.key);
    return keys;
}

SoapySDR::ArgInfo OmniCoreDevice::getSensorInfo(const std::string &key) const {
    for (auto &a: device_sensors())
        if (a.key == key) return a;
    throw std::invalid_argument("unknown sensor " + key);
}

std::string OmniCoreDevice::readSensor(const std::string &key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == "temperature") {
        auto t = och_struct<och_core_temperature>();
        check(och_get_core_temperature(dev_, &t), "get_core_temperature");
        return t.valid ? number(t.milli_celsius / 1000.0) : "nan";
    }
    if (key == "ref_locked") {
        auto r = och_struct<och_reference_status>();
        check(och_get_reference_status(dev_, &r), "get_reference_status");
        return flag(r.lock_known && r.locked == 1);
    }
    if (key == "pps_locked") {
        auto p = och_struct<och_pps_status>();
        check(och_get_pps_status(dev_, &p), "get_pps_status");
        return flag(p.lock_known && p.locked == 1);
    }
    if (key == "vcxo_locked") {
        auto v = och_struct<och_vcxo_status>();
        const int rc = och_get_vcxo_status(dev_, &v);
        if (rc == OCH_UNSUPPORTED) return flag(false);
        check(rc, "get_vcxo_status");
        return flag(v.locked == 1);
    }
    if (key == "time_valid") {
        auto t = och_struct<och_time_status>();
        check(och_get_time(dev_, &t), "get_time");
        return flag(t.valid != 0);
    }
    if (key == "usb_speed") return std::to_string(info_.usb_speed_mbps);
    throw std::invalid_argument("unknown sensor " + key);
}

std::vector<std::string> OmniCoreDevice::listSensors(int direction, size_t) const {
    if (direction == SOAPY_SDR_RX) return {"rssi"};
    return {};
}

SoapySDR::ArgInfo OmniCoreDevice::getSensorInfo(int direction, size_t, const std::string &key) const {
    if (direction == SOAPY_SDR_RX && key == "rssi")
        return arg("rssi", "RSSI", SoapySDR::ArgInfo::FLOAT,
                   "relative RSSI, higher is stronger; not calibrated power. nan while not receiving or disabled "
                   "(setting rssi_enable)",
                   "dB");
    throw std::invalid_argument("unknown sensor " + key);
}

std::string OmniCoreDevice::readSensor(int direction, size_t channel, const std::string &key) const {
    check_channel(direction, channel);
    if (direction != SOAPY_SDR_RX || key != "rssi") throw std::invalid_argument("unknown sensor " + key);
    auto r = och_struct<och_rx_rssi>();
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_get_rx_rssi(dev_, &r), "get_rx_rssi");
    return r.valid[channel] ? number(r.rssi_db[channel]) : "nan";
}

// --- Settings --------------------------------------------------------------------------------

SoapySDR::ArgInfoList OmniCoreDevice::getSettingInfo() const {
    using T = SoapySDR::ArgInfo;
    auto iq = arg("iq_bits", "USB IQ bits", T::INT, "USB sample width; 8 needs 2RX+2TX. Change only without streams",
                  "bits", {"12", "8"});
    iq.value = "12";
    auto agc = arg("rx_agc_mode", "RX AGC mode", T::STRING, "AD9361 gain control mode; manual turns AGC off", "",
                   agc_modes());
    agc.value = "slow";
    const std::vector<std::string> sources{"internal", "external"};
    auto rx_lo = arg("rx_lo_source", "RX LO source", T::STRING,
                     "external expects twice the RF frequency on the LO input", "", sources);
    auto tx_lo = arg("tx_lo_source", "TX LO source", T::STRING,
                     "external expects twice the RF frequency on the LO input", "", sources);
    rx_lo.value = tx_lo.value = "internal";
    auto pps = arg("pps_enable", "PPS monitor", T::BOOL, "PPS frequency monitor (also starts the VCXO loop)");
    auto rssi = arg("rssi_enable", "RSSI reporting", T::BOOL, "RX RSSI reporting for the rssi sensor");
    auto rgb = arg("rgb_enable", "Status LED", T::BOOL, "RGB status LED");
    auto fan = arg("fan_enable", "Fan control", T::BOOL, "temperature-controlled fan");
    pps.value = rssi.value = fan.value = "false";
    rgb.value = "true";
    return {iq, agc, rx_lo, tx_lo, pps, rssi, rgb, fan};
}

void OmniCoreDevice::writeSetting(const std::string &key, const std::string &value) {
    if (key == "iq_bits") {
        const unsigned long bits = std::stoul(value);
        if (bits != 8 && bits != 12) throw std::invalid_argument("iq_bits must be 8 or 12");
        reconfigure([&](och_config &c) {
            c.usb_iq_bits = uint32_t(bits);
            if (bits == 8) c.rx_mask = c.tx_mask = 3;
        });
        return;
    }
    if (key == "rx_agc_mode") {
        const uint32_t mode = agc_mode_index(value);
        if (mode) auto_agc_mode_ = mode;
        reconfigure([&](och_config &c) { c.rx_agc = mode; });
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == "rx_lo_source")
        check(och_set_rx_lo_source(dev_, source_index(clock_sources(), value, key.c_str())), "set_rx_lo_source");
    else if (key == "tx_lo_source")
        check(och_set_tx_lo_source(dev_, source_index(clock_sources(), value, key.c_str())), "set_tx_lo_source");
    else if (key == "pps_enable")
        check(och_set_pps_enabled(dev_, parse_bool(value)), "set_pps_enabled");
    else if (key == "rssi_enable")
        check(och_set_rx_rssi_enabled(dev_, parse_bool(value)), "set_rx_rssi_enabled");
    else if (key == "rgb_enable")
        check(och_set_rgb_enabled(dev_, parse_bool(value)), "set_rgb_enabled");
    else if (key == "fan_enable")
        check(och_set_fan_enabled(dev_, parse_bool(value)), "set_fan_enabled");
    else
        throw std::invalid_argument("unknown setting " + key);
}

std::string OmniCoreDevice::readSetting(const std::string &key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == "iq_bits") return config_.usb_iq_bits == 8 ? "8" : "12";
    if (key == "rx_agc_mode") return agc_modes().at(config_.rx_agc < agc_modes().size() ? config_.rx_agc : 0);
    if (key == "rx_lo_source" || key == "tx_lo_source") {
        auto l = och_struct<och_lo_source_config>();
        check(och_get_lo_source(dev_, &l), "get_lo_source");
        const uint32_t s = key == "rx_lo_source" ? l.rx_source : l.tx_source;
        return clock_sources().at(s ? 1 : 0);
    }
    if (key == "pps_enable") {
        auto p = och_struct<och_pps_status>();
        check(och_get_pps_status(dev_, &p), "get_pps_status");
        return flag(p.enabled != 0);
    }
    if (key == "rssi_enable") {
        auto r = och_struct<och_rx_rssi>();
        check(och_get_rx_rssi(dev_, &r), "get_rx_rssi");
        return flag(r.enabled != 0);
    }
    if (key == "rgb_enable") {
        auto r = och_struct<och_rgb_status>();
        check(och_get_rgb_status(dev_, &r), "get_rgb_status");
        return flag(r.enabled != 0);
    }
    if (key == "fan_enable") {
        auto f = och_struct<och_fan_status>();
        check(och_get_fan_status(dev_, &f), "get_fan_status");
        return flag(f.enabled != 0);
    }
    throw std::invalid_argument("unknown setting " + key);
}
