// Offline test of the shared configuration mapping (bindings/common/ConfigRequest.hpp) with 0-based
// channels and snake_case names, as the Python binding uses them. The value mapping itself is
// covered with MATLAB's names in bindings/matlab/tests/config_request_test.cpp.
#include "common/ConfigRequest.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace omnicore_bindings;

namespace {
    int failures = 0;
    void expect(bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }
    constexpr ConfigNames names{0,
                                "sample_rate",
                                "rx_frequency",
                                "tx_frequency",
                                "rx_bandwidth",
                                "tx_bandwidth",
                                "iq_bits",
                                "rx_gain",
                                "tx_gain",
                                "rx_channels",
                                "tx_channels",
                                "rx_gain_mode",
                                "(0, 1)",
                                "(a sequence of 2)",
                                "\""};
    och_config defaults() {
        och_config c{};
        c.struct_size = sizeof c;
        c.sample_rate = 30720000;
        c.rx_mask = 3;
        c.rx_frequency = c.tx_frequency = 1000000000ULL;
        c.rx_bandwidth = c.tx_bandwidth = 18000000;
        c.rx_gain[0] = c.rx_gain[1] = 30;
        c.tx_attenuation[0] = c.tx_attenuation[1] = 10;
        c.rx_agc = 2;
        return c;
    }
    // The ConfigError message, or "" when apply() accepted the request.
    std::string error(const ConfigRequest &r) {
        std::vector<std::string> w;
        try {
            apply(defaults(), r, names, w);
        } catch (const ConfigError &e) {
            return e.what();
        }
        return "";
    }
} // namespace

int main() {
    { // 0-based channel lists.
        ConfigRequest r;
        r.rx_channels = std::vector<double>{1};
        r.tx_channels = std::vector<double>{0, 1};
        std::vector<std::string> w;
        const och_config c = apply(defaults(), r, names, w);
        expect(c.rx_mask == 2 && c.tx_mask == 3, "0-based channel masks");
        expect(mask_channels(3, 0) == std::vector<double>({0, 1}) && mask_channels(2, 1) == std::vector<double>({2}),
               "masks back to lists in either numbering");
        ConfigRequest two;
        two.rx_channels = std::vector<double>{2};
        expect(error(two) == "rx_channels must list channels 0 and/or 1", "channel 2 rejected with 0-based message");
    }
    { // Messages carry the binding's names.
        ConfigRequest r;
        r.sample_rate = 1e6;
        expect(error(r).rfind("sample_rate must be", 0) == 0, "sample_rate name");
        ConfigRequest g;
        g.tx_gain = std::vector<double>{1, 2, 3};
        expect(error(g) == "tx_gain must be a scalar or one value per channel (a sequence of 2)", "per-channel hint");
        ConfigRequest m;
        m.rx_gain_mode = std::string("auto");
        expect(error(m) == "rx_gain_mode must be \"manual\", \"fast\", \"slow\" or \"hybrid\"", "gain mode quotes");
        ConfigRequest b;
        b.iq_bits = 8;
        b.tx_channels = std::vector<double>{0};
        expect(error(b) == "iq_bits 8 needs rx_channels and tx_channels (0, 1)", "8-bit channel hint");
    }
    { // A lowered gain is a plain message; the binding decides how to warn.
        ConfigRequest r;
        r.rx_frequency = 5.8e9;
        r.rx_gain = std::vector<double>{73};
        std::vector<std::string> w;
        const och_config c = apply(defaults(), r, names, w);
        expect(c.rx_gain[0] == 62 && w.size() == 2 && w[0].rfind("RX gain 73 dB lowered to 62 dB", 0) == 0,
               "gain warnings");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("config_request_test (common): all checks passed\n");
    return EXIT_SUCCESS;
}
