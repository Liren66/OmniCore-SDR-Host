// Open/close, identification, channels and RF parameters.
#include "Mapping.hpp"
#include "OmniCoreDevice.hpp"
#include <SoapySDR/Logger.hpp>
#include <cmath>

using namespace omnicore_soapy;

bool omnicore_soapy::check_supported(int rc, const char *what) {
    if (rc == OCH_UNSUPPORTED) return false;
    check(rc, what);
    return true;
}

void omnicore_soapy::check(int rc, const char *what) {
    if (rc == OCH_OK) return;
    const char *detail = och_last_error();
    throw std::runtime_error(std::string("OmniCore ") + what + ": " + (detail && *detail ? detail : "error " + std::to_string(rc)));
}

namespace {
    uint32_t parse_mask(const SoapySDR::Kwargs &args, const char *key, uint32_t fallback) {
        const auto it = args.find(key);
        if (it == args.end()) return fallback;
        const unsigned long v = std::stoul(it->second);
        if (v > 3) throw std::invalid_argument(std::string(key) + " must be 0..3");
        return uint32_t(v);
    }
} // namespace

OmniCoreDevice::OmniCoreDevice(const SoapySDR::Kwargs &args) {
    const auto serial = args.find("serial");
    check(och_open(serial != args.end() && !serial->second.empty() ? serial->second.c_str() : nullptr, &dev_),
          "open");
    try {
        info_ = och_struct<och_device_info>();
        check(och_get_info(dev_, &info_), "get_info");

        config_ = och_struct<och_config>();
        och_default_config(&config_);
        // rx_mask / tx_mask: preset both directions so gr-soapy full duplex can start its
        // second block while the first one streams (SOAPY_PLAN.md 3.4).
        config_.rx_mask = parse_mask(args, "rx_mask", config_.rx_mask);
        config_.tx_mask = parse_mask(args, "tx_mask", config_.tx_mask);
        const auto iq_bits = args.find("iq_bits");
        if (iq_bits != args.end()) {
            const unsigned long bits = std::stoul(iq_bits->second);
            if (bits != 8 && bits != 12) throw std::invalid_argument("iq_bits must be 8 or 12");
            config_.usb_iq_bits = uint32_t(bits);
            if (bits == 8) config_.rx_mask = config_.tx_mask = 3; // signed8 wire needs 2R2T
        }
        check(och_configure(dev_, &config_), "configure");
        check(och_get_config(dev_, &config_), "get_config");
        auto correction = och_struct<och_rx_correction_config>();
        rx_correction_ = check_supported(och_get_rx_correction(dev_, &correction), "get_rx_correction");

        const auto clock = args.find("clock_source");
        if (clock != args.end()) setClockSource(clock->second);
        const auto time = args.find("time_source");
        if (time != args.end()) setTimeSource(time->second);
    } catch (...) {
        och_close(dev_);
        throw;
    }
    events_ = std::thread([this] { event_loop(); });
    SoapySDR::logf(SOAPY_SDR_INFO, "OmniCore: opened %s (%s, firmware %s, USB %u Mbps)", info_.serial, info_.driver,
                   info_.firmware, info_.usb_speed_mbps);
}

OmniCoreDevice::~OmniCoreDevice() {
    // Applications should close their streams first; close any left so och_close does not fail.
    for (OmniCoreStream *s: {rx_stream_, tx_stream_})
        if (s) closeStream(reinterpret_cast<SoapySDR::Stream *>(s));
    stop_events_ = true;
    if (events_.joinable()) events_.join();
    if (och_close(dev_) != OCH_OK) SoapySDR::logf(SOAPY_SDR_ERROR, "OmniCore: close failed: %s", och_last_error());
}

void OmniCoreDevice::reconfigure(const std::function<void(och_config &)> &change) {
    std::lock_guard<std::mutex> lock(mutex_);
    apply_locked(change);
}

void OmniCoreDevice::apply_locked(const std::function<void(och_config &)> &change) {
    och_config next = config_;
    change(next);
    check(och_configure(dev_, &next), "configure");
    check(och_get_config(dev_, &next), "get_config");
    config_ = next;
}

void OmniCoreDevice::check_channel(int direction, size_t channel) const {
    if (direction != SOAPY_SDR_RX && direction != SOAPY_SDR_TX) throw std::invalid_argument("invalid direction");
    if (channel >= getNumChannels(direction)) throw std::invalid_argument("channel " + std::to_string(channel) + " does not exist");
}

// --- Identification -------------------------------------------------------------------------

std::string OmniCoreDevice::getDriverKey() const { return "omnicore"; }
std::string OmniCoreDevice::getHardwareKey() const { return "OmniCoreSDR"; }

SoapySDR::Kwargs OmniCoreDevice::getHardwareInfo() const {
    return {{"serial", info_.serial},
            {"name", info_.name},
            {"firmware", info_.firmware},
            {"usb_driver", info_.driver},
            {"usb_speed_mbps", std::to_string(info_.usb_speed_mbps)},
            {"och_version", och_version()}};
}

// --- Channels -------------------------------------------------------------------------------

size_t OmniCoreDevice::getNumChannels(int direction) const {
    return direction == SOAPY_SDR_RX ? info_.rx_channels : direction == SOAPY_SDR_TX ? info_.tx_channels : 0;
}
bool OmniCoreDevice::getFullDuplex(int, size_t) const { return true; }

// Each channel has one fixed RF port.
std::vector<std::string> OmniCoreDevice::listAntennas(int direction, size_t) const {
    return {direction == SOAPY_SDR_RX ? "RX" : "TX"};
}
void OmniCoreDevice::setAntenna(int direction, size_t channel, const std::string &name) {
    check_channel(direction, channel);
    if (name != getAntenna(direction, channel)) throw std::invalid_argument("unknown antenna " + name);
}
std::string OmniCoreDevice::getAntenna(int direction, size_t) const { return direction == SOAPY_SDR_RX ? "RX" : "TX"; }

// --- Front-end corrections (RX only, shared by both receivers) ------------------------------

bool OmniCoreDevice::hasDCOffsetMode(int direction, size_t) const { return direction == SOAPY_SDR_RX && rx_correction_; }
void OmniCoreDevice::setDCOffsetMode(int direction, size_t channel, bool automatic) {
    check_channel(direction, channel);
    // gr-soapy turns the mode off even on channels without it; off is then already true.
    if (!automatic && !hasDCOffsetMode(direction, channel)) return;
    if (!hasDCOffsetMode(direction, channel)) throw std::invalid_argument("automatic DC correction is not available");
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_set_rx_dc_correction(dev_, automatic), "set_rx_dc_correction");
}
bool OmniCoreDevice::getDCOffsetMode(int direction, size_t) const {
    if (direction != SOAPY_SDR_RX || !rx_correction_) return false;
    auto c = och_struct<och_rx_correction_config>();
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_get_rx_correction(dev_, &c), "get_rx_correction");
    return c.dc_enabled != 0;
}
bool OmniCoreDevice::hasIQBalanceMode(int direction, size_t) const { return direction == SOAPY_SDR_RX && rx_correction_; }
void OmniCoreDevice::setIQBalanceMode(int direction, size_t channel, bool automatic) {
    check_channel(direction, channel);
    if (!automatic && !hasIQBalanceMode(direction, channel)) return;
    if (!hasIQBalanceMode(direction, channel)) throw std::invalid_argument("automatic IQ correction is not available");
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_set_rx_iq_correction(dev_, automatic), "set_rx_iq_correction");
}
bool OmniCoreDevice::getIQBalanceMode(int direction, size_t) const {
    if (direction != SOAPY_SDR_RX || !rx_correction_) return false;
    auto c = och_struct<och_rx_correction_config>();
    std::lock_guard<std::mutex> lock(mutex_);
    check(och_get_rx_correction(dev_, &c), "get_rx_correction");
    return c.iq_enabled != 0;
}

// --- Gain: one element "PGA" per channel -----------------------------------------------------

std::vector<std::string> OmniCoreDevice::listGains(int, size_t) const { return {"PGA"}; }

bool OmniCoreDevice::hasGainMode(int direction, size_t) const { return direction == SOAPY_SDR_RX; }
void OmniCoreDevice::setGainMode(int direction, size_t channel, bool automatic) {
    check_channel(direction, channel);
    // gr-soapy calls setGainMode(false) on every channel, TX included.
    if (direction != SOAPY_SDR_RX) {
        if (automatic) throw std::invalid_argument("AGC is RX only");
        return;
    }
    const uint32_t mode = automatic ? auto_agc_mode_ : 0;
    reconfigure([&](och_config &c) { c.rx_agc = mode; });
}
bool OmniCoreDevice::getGainMode(int direction, size_t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return direction == SOAPY_SDR_RX && config_.rx_agc != 0;
}

void OmniCoreDevice::setGain(int direction, size_t channel, double value) { setGain(direction, channel, "PGA", value); }
void OmniCoreDevice::setGain(int direction, size_t channel, const std::string &name, double value) {
    check_channel(direction, channel);
    if (name != "PGA") throw std::invalid_argument("unknown gain element " + name);
    if (direction == SOAPY_SDR_RX)
        reconfigure([&](och_config &c) { c.rx_gain[channel] = rx_gain_to_config(value, double(c.rx_frequency)); });
    else
        reconfigure([&](och_config &c) { c.tx_attenuation[channel] = tx_gain_to_attenuation(value); });
}
double OmniCoreDevice::getGain(int direction, size_t channel) const { return getGain(direction, channel, "PGA"); }
double OmniCoreDevice::getGain(int direction, size_t channel, const std::string &) const {
    check_channel(direction, channel);
    std::lock_guard<std::mutex> lock(mutex_);
    return direction == SOAPY_SDR_RX ? config_.rx_gain[channel] : tx_attenuation_to_gain(config_.tx_attenuation[channel]);
}
SoapySDR::Range OmniCoreDevice::getGainRange(int direction, size_t channel) const {
    return getGainRange(direction, channel, "PGA");
}
SoapySDR::Range OmniCoreDevice::getGainRange(int direction, size_t, const std::string &) const {
    if (direction == SOAPY_SDR_RX) {
        std::lock_guard<std::mutex> lock(mutex_);
        return SoapySDR::Range(rx_gain_min, rx_gain_max(double(config_.rx_frequency)), rx_gain_step);
    }
    return SoapySDR::Range(0, tx_attenuation_max, tx_gain_step);
}

// --- Frequency: one LO per direction, shared by both channels --------------------------------

void OmniCoreDevice::setFrequency(int direction, size_t channel, double frequency, const SoapySDR::Kwargs &args) {
    setFrequency(direction, channel, "RF", frequency, args);
}
void OmniCoreDevice::setFrequency(int direction, size_t channel, const std::string &name, double frequency,
                                  const SoapySDR::Kwargs &) {
    check_channel(direction, channel);
    if (name != "RF") throw std::invalid_argument("unknown frequency element " + name);
    const auto hz = uint64_t(std::llround(clamp(frequency, min_frequency, max_frequency)));
    if (direction == SOAPY_SDR_TX) {
        reconfigure([&](och_config &c) { c.tx_frequency = hz; });
        return;
    }
    // A gain that was valid in the old band can be too high for the new one; lower it in the same
    // configure so the device never rejects it.
    reconfigure([&](och_config &c) {
        c.rx_frequency = hz;
        for (auto &g : c.rx_gain) {
            const double fitted = rx_gain_to_config(g, double(hz));
            if (fitted != g)
                SoapySDR::logf(SOAPY_SDR_INFO, "OmniCore: RX gain %.0f dB lowered to %.0f dB, the maximum at %.3f MHz",
                               g, fitted, double(hz) / 1e6);
            g = fitted;
        }
    });
}
double OmniCoreDevice::getFrequency(int direction, size_t channel) const { return getFrequency(direction, channel, "RF"); }
double OmniCoreDevice::getFrequency(int direction, size_t channel, const std::string &) const {
    check_channel(direction, channel);
    std::lock_guard<std::mutex> lock(mutex_);
    return double(direction == SOAPY_SDR_RX ? config_.rx_frequency : config_.tx_frequency);
}
std::vector<std::string> OmniCoreDevice::listFrequencies(int, size_t) const { return {"RF"}; }
SoapySDR::RangeList OmniCoreDevice::getFrequencyRange(int direction, size_t channel) const {
    return getFrequencyRange(direction, channel, "RF");
}
SoapySDR::RangeList OmniCoreDevice::getFrequencyRange(int, size_t, const std::string &) const {
    return {SoapySDR::Range(min_frequency, max_frequency)};
}

// --- Sample rate (shared by RX, TX and both channels) and bandwidth (per direction) ---------

void OmniCoreDevice::setSampleRate(int direction, size_t channel, double rate) {
    check_channel(direction, channel);
    if (!(rate >= min_sample_rate && rate <= max_sample_rate))
        throw std::invalid_argument("sample rate must be " + std::to_string(min_sample_rate) + ".." +
                                    std::to_string(max_sample_rate) + " Hz");
    reconfigure([&](och_config &c) { c.sample_rate = uint32_t(std::llround(rate)); });
    std::lock_guard<std::mutex> lock(mutex_);
    stream_rate_changed();
    if (std::fabs(config_.sample_rate - rate) >= 1)
        SoapySDR::logf(SOAPY_SDR_INFO, "OmniCore: %.0f Hz is not exact on the AD9361 clock chain; using %u Hz", rate,
                       config_.sample_rate);
}
double OmniCoreDevice::getSampleRate(int, size_t) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.sample_rate;
}
SoapySDR::RangeList OmniCoreDevice::getSampleRateRange(int, size_t) const {
    return {SoapySDR::Range(min_sample_rate, max_sample_rate)};
}

void OmniCoreDevice::setBandwidth(int direction, size_t channel, double bandwidth) {
    check_channel(direction, channel);
    // 0 means "no preference": GRC's Soapy blocks always pass it by default.
    if (!(bandwidth > 0)) return;
    const auto hz = uint32_t(std::llround(clamp(bandwidth, min_bandwidth, max_bandwidth)));
    reconfigure([&](och_config &c) { (direction == SOAPY_SDR_RX ? c.rx_bandwidth : c.tx_bandwidth) = hz; });
}
double OmniCoreDevice::getBandwidth(int direction, size_t channel) const {
    check_channel(direction, channel);
    std::lock_guard<std::mutex> lock(mutex_);
    return direction == SOAPY_SDR_RX ? config_.rx_bandwidth : config_.tx_bandwidth;
}
SoapySDR::RangeList OmniCoreDevice::getBandwidthRange(int, size_t) const {
    return {SoapySDR::Range(min_bandwidth, max_bandwidth)};
}
