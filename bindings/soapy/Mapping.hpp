// Pure value mappings between SoapySDR conventions and och_config. No SoapySDR or device
// dependency, so tests/mapping_test.cpp can check them offline.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace omnicore_soapy {
    // Sample rate range offered to applications. The lower bound is a conservative value above
    // the AD9361 no-FIR minimum (about 2.083 MSPS); lower rates need FIR decimation and are not
    // advertised until they are verified on hardware.
    constexpr double min_sample_rate = 2.5e6;
    constexpr double max_sample_rate = 61.44e6;

    constexpr double min_frequency = 70e6, max_frequency = 6e9;
    constexpr double min_bandwidth = 200e3, max_bandwidth = 56e6;

    // RX: integer dB. The core accepts -3..76, but the AD9361 gain table caps it by RX LO band;
    // a rejected gain makes the core stop every stream, so the module never sends one.
    // Limits measured on hardware (SOAPY_PLAN.md 8.4).
    constexpr double rx_gain_min = -3, rx_gain_step = 1;
    inline double rx_gain_max(double rx_lo_hz) {
        return rx_lo_hz <= 1300e6 ? 73 : rx_lo_hz <= 4000e6 ? 71 : 62;
    }
    // TX follows SoapyPlutoSDR: gain = max attenuation - attenuation, 0.25 dB steps.
    constexpr double tx_attenuation_max = 89.75, tx_gain_step = 0.25;

    inline double clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

    inline double rx_gain_to_config(double gain, double rx_lo_hz) {
        return std::round(clamp(gain, rx_gain_min, rx_gain_max(rx_lo_hz)));
    }

    inline double tx_gain_to_attenuation(double gain) {
        const double g = std::round(clamp(gain, 0, tx_attenuation_max) / tx_gain_step) * tx_gain_step;
        return tx_attenuation_max - g;
    }
    inline double tx_attenuation_to_gain(double attenuation) { return tx_attenuation_max - attenuation; }

    // Stream channel list -> och_config channel mask (1 = channel 0, 2 = channel 1, 3 = both).
    inline uint32_t channels_to_mask(const std::vector<size_t> &channels, size_t available) {
        if (channels.empty()) return 1;
        uint32_t mask = 0;
        for (size_t i = 0; i < channels.size(); ++i) {
            const size_t ch = channels[i];
            if (ch >= available || ch > 1) throw std::invalid_argument("channel " + std::to_string(ch) + " does not exist");
            if (mask & (1u << ch)) throw std::invalid_argument("channel " + std::to_string(ch) + " listed twice");
            // och interleaves channels in ascending order; a reversed list would need remapping.
            if (i > 0 && ch < channels[i - 1]) throw std::invalid_argument("list channels in ascending order");
            mask |= 1u << ch;
        }
        return mask;
    }

    // och_config.rx_agc values.
    inline const std::vector<std::string> &agc_modes() {
        static const std::vector<std::string> modes{"manual", "fast", "slow", "hybrid"};
        return modes;
    }
    inline uint32_t agc_mode_index(const std::string &name) {
        const auto &m = agc_modes();
        for (size_t i = 0; i < m.size(); ++i)
            if (m[i] == name) return uint32_t(i);
        throw std::invalid_argument("rx_agc_mode must be manual, fast, slow or hybrid");
    }

    // Clock (10 MHz reference) and time (PPS) source names, indexed by their och values.
    inline const std::vector<std::string> &clock_sources() {
        static const std::vector<std::string> s{"internal", "external"};
        return s;
    }
    inline const std::vector<std::string> &time_sources() {
        static const std::vector<std::string> s{"none", "external", "internal", "sync"};
        return s;
    }
    inline uint32_t source_index(const std::vector<std::string> &names, const std::string &name, const char *what) {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == name) return uint32_t(i);
        std::string all;
        for (auto &n: names) all += (all.empty() ? "" : ", ") + n;
        throw std::invalid_argument(std::string(what) + " must be one of: " + all);
    }

    inline bool parse_bool(const std::string &v) {
        if (v == "true" || v == "1" || v == "on") return true;
        if (v == "false" || v == "0" || v == "off") return false;
        throw std::invalid_argument("expected true or false, got '" + v + "'");
    }
} // namespace omnicore_soapy
