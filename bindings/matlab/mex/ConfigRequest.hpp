// MATLAB-side RF parameters (MEX_PLAN.md 3.1) -> one och_config. Pure logic without MATLAB or a
// device, tested offline in tests/config_request_test.cpp.
//
// A request names only the fields to change; apply() turns it into the next och_config so the
// device sees a single och_configure (one stream pause for a rate and bandwidth change). Values
// outside the device range are errors, except an RX gain above the maximum of the RX LO band,
// which is lowered with a warning (the core would otherwise reject it and stop every stream).
#pragma once
#include "Error.hpp"
#include "common/Mapping.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace omnicore_mex {
    struct ConfigRequest {
        std::optional<double> sample_rate, rx_frequency, tx_frequency, rx_bandwidth, tx_bandwidth, iq_bits;
        std::optional<std::vector<double>> rx_gain, tx_gain;         // scalar (both channels) or one per channel
        std::optional<std::vector<double>> rx_channels, tx_channels; // 1-based, ascending; empty = off
        std::optional<std::string> rx_gain_mode;

        bool empty() const {
            return !sample_rate && !rx_frequency && !tx_frequency && !rx_bandwidth && !tx_bandwidth && !iq_bits &&
                   !rx_gain && !tx_gain && !rx_channels && !tx_channels && !rx_gain_mode;
        }
    };

    namespace detail {
        inline std::string hz(double v) {
            char b[32];
            std::snprintf(b, sizeof b, "%.6g", v);
            return b;
        }
        inline void check_range(double v, double lo, double hi, const char *what, const char *unit) {
            if (!(v >= lo && v <= hi))
                throw invalid_argument(std::string(what) + " must be " + hz(lo) + " to " + hz(hi) + " " + unit);
        }
        // 1-based MATLAB channel list -> och mask (1 = channel 1, 2 = channel 2, 3 = both).
        inline uint32_t channel_mask(const std::vector<double> &channels, const char *what) {
            std::vector<size_t> zero_based;
            for (double c: channels) {
                if (c != std::floor(c) || c < 1 || c > 2)
                    throw invalid_argument(std::string(what) + " must list channels 1 and/or 2");
                zero_based.push_back(size_t(c) - 1);
            }
            if (zero_based.empty()) return 0;
            try {
                return omnicore_bindings::channels_to_mask(zero_based, 2);
            } catch (const std::exception &e) {
                throw invalid_argument(std::string(what) + ": " + e.what());
            }
        }
        inline std::vector<double> per_channel(const std::vector<double> &v, const char *what) {
            if (v.size() == 1) return {v[0], v[0]};
            if (v.size() == 2) return v;
            throw invalid_argument(std::string(what) + " must be a scalar or one value per channel (1x2)");
        }
        inline double fit_rx_gain(double gain, double rx_lo_hz, Warnings &warnings) {
            using namespace omnicore_bindings;
            const double max = rx_gain_max(rx_lo_hz);
            const double rounded = std::round(gain);
            if (rounded <= max) return rounded;
            char b[160];
            std::snprintf(b, sizeof b, "RX gain %.0f dB lowered to %.0f dB, the maximum at %.3f MHz", rounded, max,
                          rx_lo_hz / 1e6);
            warnings.push_back({"omnicore:GainLimited", b});
            return max;
        }
    } // namespace detail

    // Channel masks back to the 1-based list MATLAB shows.
    inline std::vector<double> mask_channels(uint32_t mask) {
        std::vector<double> out;
        if (mask & 1) out.push_back(1);
        if (mask & 2) out.push_back(2);
        return out;
    }

    inline och_config apply(const och_config &current, const ConfigRequest &r, Warnings &warnings) {
        using namespace omnicore_bindings;
        och_config c = current;

        if (r.rx_channels) c.rx_mask = detail::channel_mask(*r.rx_channels, "RxChannels");
        if (r.tx_channels) c.tx_mask = detail::channel_mask(*r.tx_channels, "TxChannels");
        if (r.iq_bits) {
            if (*r.iq_bits != 8 && *r.iq_bits != 12) throw invalid_argument("IQBits must be 8 or 12");
            c.usb_iq_bits = uint32_t(*r.iq_bits);
            // The signed8 wire format carries both channels in both directions.
            if (c.usb_iq_bits == 8) {
                if (!r.rx_channels) c.rx_mask = 3;
                if (!r.tx_channels) c.tx_mask = 3;
            }
        }
        if (c.usb_iq_bits == 8 && (c.rx_mask != 3 || c.tx_mask != 3))
            throw invalid_argument("IQBits 8 needs RxChannels and TxChannels [1 2]");

        if (r.sample_rate) {
            detail::check_range(*r.sample_rate, min_sample_rate, max_sample_rate, "SampleRate", "Hz");
            c.sample_rate = uint32_t(std::llround(*r.sample_rate));
        }
        if (r.rx_bandwidth) {
            detail::check_range(*r.rx_bandwidth, min_bandwidth, max_bandwidth, "RxBandwidth", "Hz");
            c.rx_bandwidth = uint32_t(std::llround(*r.rx_bandwidth));
        }
        if (r.tx_bandwidth) {
            detail::check_range(*r.tx_bandwidth, min_bandwidth, max_bandwidth, "TxBandwidth", "Hz");
            c.tx_bandwidth = uint32_t(std::llround(*r.tx_bandwidth));
        }
        if (r.rx_frequency) {
            detail::check_range(*r.rx_frequency, min_frequency, max_frequency, "RxFrequency", "Hz");
            c.rx_frequency = uint64_t(std::llround(*r.rx_frequency));
        }
        if (r.tx_frequency) {
            detail::check_range(*r.tx_frequency, min_frequency, max_frequency, "TxFrequency", "Hz");
            c.tx_frequency = uint64_t(std::llround(*r.tx_frequency));
        }
        if (r.rx_gain_mode) {
            try {
                c.rx_agc = agc_mode_index(*r.rx_gain_mode);
            } catch (const std::exception &) {
                throw invalid_argument("RxGainMode must be 'manual', 'fast', 'slow' or 'hybrid'");
            }
        }

        // Gains last, against the RX LO of this same request.
        const double rx_lo = double(c.rx_frequency);
        if (r.rx_gain) {
            const auto g = detail::per_channel(*r.rx_gain, "RxGain");
            for (size_t i = 0; i < 2; ++i) {
                if (g[i] < rx_gain_min) throw invalid_argument("RxGain must be at least -3 dB");
                c.rx_gain[i] = detail::fit_rx_gain(g[i], rx_lo, warnings);
            }
        } else if (c.rx_frequency != current.rx_frequency) {
            // A gain valid in the old band can be too high for the new one.
            for (auto &g: c.rx_gain) g = detail::fit_rx_gain(g, rx_lo, warnings);
        }
        if (r.tx_gain) {
            const auto g = detail::per_channel(*r.tx_gain, "TxGain");
            for (size_t i = 0; i < 2; ++i) {
                detail::check_range(g[i], 0, tx_attenuation_max, "TxGain", "dB");
                c.tx_attenuation[i] = tx_gain_to_attenuation(g[i]);
            }
        }
        return c;
    }
} // namespace omnicore_mex
