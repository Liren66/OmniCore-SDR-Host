// RF parameters of a binding (MEX_PLAN.md 3.1, PYTHON_PLAN.md 3.1) -> one och_config. Pure logic
// without MATLAB, Python or a device, tested offline in tests/config_request_test.cpp and, with
// MATLAB's names, bindings/matlab/tests/config_request_test.cpp.
//
// A request names only the fields to change; apply() turns it into the next och_config so the
// device sees a single och_configure (one stream pause for a rate and bandwidth change). Values
// outside the device range are errors (ConfigError), except an RX gain above the maximum of the RX
// LO band, which is lowered and reported in `gain_warnings` (the core would otherwise reject it
// and stop every stream). Messages use the binding's own parameter names and channel numbering
// (ConfigNames).
#pragma once
#include "common/Mapping.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <omnicore/och.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace omnicore_bindings {
    struct ConfigRequest {
        std::optional<double> sample_rate, rx_frequency, tx_frequency, rx_bandwidth, tx_bandwidth, iq_bits;
        std::optional<std::vector<double>> rx_gain, tx_gain;         // scalar (both channels) or one per channel
        std::optional<std::vector<double>> rx_channels, tx_channels; // ascending, from channel_base; empty = off
        std::optional<std::string> rx_gain_mode;

        bool empty() const {
            return !sample_rate && !rx_frequency && !tx_frequency && !rx_bandwidth && !tx_bandwidth && !iq_bits &&
                   !rx_gain && !tx_gain && !rx_channels && !tx_channels && !rx_gain_mode;
        }
    };

    // How a binding names the parameters in its messages.
    struct ConfigNames {
        int channel_base; // number of the first channel: MATLAB 1, Python 0
        const char *sample_rate, *rx_frequency, *tx_frequency, *rx_bandwidth, *tx_bandwidth, *iq_bits;
        const char *rx_gain, *tx_gain, *rx_channels, *tx_channels, *rx_gain_mode;
        const char *both_channels; // the value that selects both channels: "[1 2]", "(0, 1)"
        const char *per_channel;   // how one value per channel is written: "(1x2)", "(a sequence of 2)"
        const char *quote;         // around gain mode names in messages
    };

    // A request the device would reject.
    class ConfigError : public std::invalid_argument {
    public:
        using std::invalid_argument::invalid_argument;
    };

    namespace detail {
        inline std::string hz(double v) {
            char b[32];
            std::snprintf(b, sizeof b, "%.6g", v);
            return b;
        }
        inline void check_range(double v, double lo, double hi, const char *what, const char *unit) {
            if (!(v >= lo && v <= hi))
                throw ConfigError(std::string(what) + " must be " + hz(lo) + " to " + hz(hi) + " " + unit);
        }
        // Channel list in the binding's numbering -> och mask (1 = first channel, 2 = second, 3 = both).
        inline uint32_t channel_mask(const std::vector<double> &channels, const char *what, int base) {
            std::vector<size_t> zero_based;
            for (double c: channels) {
                if (c != std::floor(c) || c < base || c > base + 1)
                    throw ConfigError(std::string(what) + " must list channels " + std::to_string(base) + " and/or " +
                                      std::to_string(base + 1));
                zero_based.push_back(size_t(c - base));
            }
            if (zero_based.empty()) return 0;
            try {
                return channels_to_mask(zero_based, 2);
            } catch (const std::exception &e) {
                throw ConfigError(std::string(what) + ": " + e.what());
            }
        }
        inline std::vector<double> per_channel(const std::vector<double> &v, const char *what, const ConfigNames &n) {
            if (v.size() == 1) return {v[0], v[0]};
            if (v.size() == 2) return v;
            throw ConfigError(std::string(what) + " must be a scalar or one value per channel " + n.per_channel);
        }
        inline double fit_rx_gain(double gain, double rx_lo_hz, std::vector<std::string> &warnings) {
            const double max = rx_gain_max(rx_lo_hz);
            const double rounded = std::round(gain);
            if (rounded <= max) return rounded;
            char b[160];
            std::snprintf(b, sizeof b, "RX gain %.0f dB lowered to %.0f dB, the maximum at %.3f MHz", rounded, max,
                          rx_lo_hz / 1e6);
            warnings.push_back(b);
            return max;
        }
    } // namespace detail

    // Channel mask back to the list the binding shows.
    inline std::vector<double> mask_channels(uint32_t mask, int base) {
        std::vector<double> out;
        if (mask & 1) out.push_back(base);
        if (mask & 2) out.push_back(base + 1);
        return out;
    }

    // Throws ConfigError; each RX gain lowered to the band maximum adds one message to gain_warnings.
    inline och_config apply(const och_config &current, const ConfigRequest &r, const ConfigNames &n,
                            std::vector<std::string> &gain_warnings) {
        och_config c = current;
        const std::string q = n.quote;

        if (r.rx_channels) c.rx_mask = detail::channel_mask(*r.rx_channels, n.rx_channels, n.channel_base);
        if (r.tx_channels) c.tx_mask = detail::channel_mask(*r.tx_channels, n.tx_channels, n.channel_base);
        if (r.iq_bits) {
            if (*r.iq_bits != 8 && *r.iq_bits != 12) throw ConfigError(std::string(n.iq_bits) + " must be 8 or 12");
            c.usb_iq_bits = uint32_t(*r.iq_bits);
            // The signed8 wire format carries both channels in both directions.
            if (c.usb_iq_bits == 8) {
                if (!r.rx_channels) c.rx_mask = 3;
                if (!r.tx_channels) c.tx_mask = 3;
            }
        }
        if (!c.rx_mask && !c.tx_mask)
            throw ConfigError(std::string(n.rx_channels) + " and " + n.tx_channels + " cannot both be empty");
        if (c.usb_iq_bits == 8 && (c.rx_mask != 3 || c.tx_mask != 3))
            throw ConfigError(std::string(n.iq_bits) + " 8 needs " + n.rx_channels + " and " + n.tx_channels + " " +
                              n.both_channels);

        if (r.sample_rate) {
            detail::check_range(*r.sample_rate, min_sample_rate, max_sample_rate, n.sample_rate, "Hz");
            c.sample_rate = uint32_t(std::llround(*r.sample_rate));
        }
        if (r.rx_bandwidth) {
            detail::check_range(*r.rx_bandwidth, min_bandwidth, max_bandwidth, n.rx_bandwidth, "Hz");
            c.rx_bandwidth = uint32_t(std::llround(*r.rx_bandwidth));
        }
        if (r.tx_bandwidth) {
            detail::check_range(*r.tx_bandwidth, min_bandwidth, max_bandwidth, n.tx_bandwidth, "Hz");
            c.tx_bandwidth = uint32_t(std::llround(*r.tx_bandwidth));
        }
        if (r.rx_frequency) {
            detail::check_range(*r.rx_frequency, min_frequency, max_frequency, n.rx_frequency, "Hz");
            c.rx_frequency = uint64_t(std::llround(*r.rx_frequency));
        }
        if (r.tx_frequency) {
            detail::check_range(*r.tx_frequency, min_frequency, max_frequency, n.tx_frequency, "Hz");
            c.tx_frequency = uint64_t(std::llround(*r.tx_frequency));
        }
        if (r.rx_gain_mode) {
            try {
                c.rx_agc = agc_mode_index(*r.rx_gain_mode);
            } catch (const std::exception &) {
                throw ConfigError(std::string(n.rx_gain_mode) + " must be " + q + "manual" + q + ", " + q + "fast" + q +
                                  ", " + q + "slow" + q + " or " + q + "hybrid" + q);
            }
        }

        // Gains last, against the RX LO of this same request.
        const double rx_lo = double(c.rx_frequency);
        if (r.rx_gain) {
            const auto g = detail::per_channel(*r.rx_gain, n.rx_gain, n);
            for (size_t i = 0; i < 2; ++i) {
                if (g[i] < rx_gain_min) throw ConfigError(std::string(n.rx_gain) + " must be at least -3 dB");
                c.rx_gain[i] = detail::fit_rx_gain(g[i], rx_lo, gain_warnings);
            }
        } else if (c.rx_frequency != current.rx_frequency) {
            // A gain valid in the old band can be too high for the new one.
            for (auto &g: c.rx_gain) g = detail::fit_rx_gain(g, rx_lo, gain_warnings);
        }
        if (r.tx_gain) {
            const auto g = detail::per_channel(*r.tx_gain, n.tx_gain, n);
            for (size_t i = 0; i < 2; ++i) {
                detail::check_range(g[i], 0, tx_attenuation_max, n.tx_gain, "dB");
                c.tx_attenuation[i] = tx_gain_to_attenuation(g[i]);
            }
        }
        return c;
    }
} // namespace omnicore_bindings
