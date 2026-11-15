// MATLAB-side RF parameters (MEX_PLAN.md 3.1) -> one och_config: the shared mapping of
// common/ConfigRequest.hpp with MATLAB's property names, 1-based channels and error / warning
// identifiers. Tested offline in tests/config_request_test.cpp.
#pragma once
#include "Error.hpp"
#include "common/ConfigRequest.hpp"
#include <string>
#include <vector>

namespace omnicore_mex {
    using omnicore_bindings::ConfigRequest;

    inline constexpr omnicore_bindings::ConfigNames matlab_config_names{
            1,        "SampleRate", "RxFrequency", "TxFrequency", "RxBandwidth", "TxBandwidth", "IQBits", "RxGain",
            "TxGain", "RxChannels", "TxChannels",  "RxGainMode",  "[1 2]",       "(1x2)",       "'"};

    // Channel masks back to the 1-based list MATLAB shows.
    inline std::vector<double> mask_channels(uint32_t mask) { return omnicore_bindings::mask_channels(mask, 1); }

    // Errors are omnicore:InvalidArgument; a lowered RX gain is an omnicore:GainLimited warning.
    inline och_config apply(const och_config &current, const ConfigRequest &r, Warnings &warnings) {
        std::vector<std::string> limited;
        och_config c;
        try {
            c = omnicore_bindings::apply(current, r, matlab_config_names, limited);
        } catch (const omnicore_bindings::ConfigError &e) {
            throw invalid_argument(e.what());
        }
        for (auto &m: limited) warnings.push_back({"omnicore:GainLimited", std::move(m)});
        return c;
    }
} // namespace omnicore_mex
