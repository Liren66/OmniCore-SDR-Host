// Offline test of the MATLAB configuration mapping (bindings/matlab/mex/ConfigRequest.hpp).
#include "ConfigRequest.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace omnicore_mex;

namespace {
    int failures = 0;
    void expect(bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL: %s\n", what);
            ++failures;
        }
    }
    // True when apply() throws omnicore:InvalidArgument.
    bool rejects(const och_config &base, const ConfigRequest &r) {
        Warnings w;
        try {
            apply(base, r, w);
        } catch (const Error &e) {
            return e.id() == "omnicore:InvalidArgument";
        }
        return false;
    }
    och_config defaults() {
        och_config c{};
        c.struct_size = sizeof c;
        c.sample_rate = 30720000;
        c.rx_mask = 3;
        c.tx_mask = 0;
        c.rx_frequency = c.tx_frequency = 1000000000ULL;
        c.rx_bandwidth = c.tx_bandwidth = 18000000;
        c.rx_gain[0] = c.rx_gain[1] = 30;
        c.tx_attenuation[0] = c.tx_attenuation[1] = 10;
        c.rx_agc = 2;
        return c;
    }
} // namespace

int main() {
    const och_config base = defaults();

    { // Empty request changes nothing.
        Warnings w;
        const och_config c = apply(base, ConfigRequest{}, w);
        expect(ConfigRequest{}.empty(), "empty request");
        expect(c.sample_rate == base.sample_rate && c.rx_mask == 3 && w.empty(), "no-op apply");
    }
    { // Plain values, scalar gains expand to both channels, TX gain = 89.75 - attenuation.
        ConfigRequest r;
        r.sample_rate = 61.44e6;
        r.rx_frequency = 2.45e9;
        r.tx_bandwidth = 56e6;
        r.rx_gain = std::vector<double>{40.4};
        r.tx_gain = std::vector<double>{80, 70.1};
        r.rx_gain_mode = "manual";
        Warnings w;
        const och_config c = apply(base, r, w);
        expect(c.sample_rate == 61440000 && c.rx_frequency == 2450000000ULL && c.tx_bandwidth == 56000000,
               "plain values");
        expect(c.rx_gain[0] == 40 && c.rx_gain[1] == 40, "RX gain rounded and expanded");
        expect(c.tx_attenuation[0] == 9.75 && c.tx_attenuation[1] == 19.75, "TX gain to attenuation, 0.25 dB steps");
        expect(c.rx_agc == 0 && w.empty(), "manual gain mode");
    }
    { // RX gain above the band maximum is lowered with a warning, in the band of the same request.
        ConfigRequest r;
        r.rx_frequency = 5.8e9;
        r.rx_gain = std::vector<double>{73, 50};
        Warnings w;
        const och_config c = apply(base, r, w);
        expect(c.rx_gain[0] == 62 && c.rx_gain[1] == 50, "gain capped at 62 dB above 4 GHz");
        expect(w.size() == 1 && w[0].id == "omnicore:GainLimited", "one GainLimited warning");
    }
    { // A frequency change alone lowers existing gains that no longer fit.
        och_config high = base;
        high.rx_gain[0] = high.rx_gain[1] = 73;
        ConfigRequest r;
        r.rx_frequency = 2.4e9;
        Warnings w;
        const och_config c = apply(high, r, w);
        expect(c.rx_gain[0] == 71 && c.rx_gain[1] == 71 && w.size() == 2, "existing gains follow the band");
    }
    { // Channels: 1-based lists to masks, empty = off.
        ConfigRequest r;
        r.rx_channels = std::vector<double>{2};
        r.tx_channels = std::vector<double>{1, 2};
        Warnings w;
        const och_config c = apply(base, r, w);
        expect(c.rx_mask == 2 && c.tx_mask == 3, "channel masks");
        ConfigRequest off;
        off.rx_channels = std::vector<double>{};
        expect(apply(c, off, w).rx_mask == 0, "empty channel list disables the direction (TX still on)");
        expect(mask_channels(3) == std::vector<double>({1, 2}) && mask_channels(0).empty(), "masks back to lists");
    }
    { // IQBits 8 forces both directions to both channels, and rejects anything else.
        ConfigRequest r;
        r.iq_bits = 8;
        Warnings w;
        const och_config c = apply(base, r, w);
        expect(c.usb_iq_bits == 8 && c.rx_mask == 3 && c.tx_mask == 3, "8-bit sets 2R2T");
        ConfigRequest bad = r;
        bad.tx_channels = std::vector<double>{1};
        expect(rejects(base, bad), "8-bit with one TX channel");
        ConfigRequest later;
        later.rx_channels = std::vector<double>{1};
        expect(rejects(c, later), "channel change while in 8-bit mode");
    }
    { // Out-of-range values are errors, not silent clamps.
        auto one = [&](auto set) {
            ConfigRequest r;
            set(r);
            return rejects(base, r);
        };
        expect(one([](ConfigRequest &r) { r.sample_rate = 1e6; }), "sample rate too low");
        expect(one([](ConfigRequest &r) { r.sample_rate = 70e6; }), "sample rate too high");
        expect(one([](ConfigRequest &r) { r.rx_frequency = 50e6; }), "frequency too low");
        expect(one([](ConfigRequest &r) { r.tx_frequency = 6.1e9; }), "frequency too high");
        expect(one([](ConfigRequest &r) { r.rx_bandwidth = 100e3; }), "bandwidth too low");
        expect(one([](ConfigRequest &r) { r.rx_gain = std::vector<double>{-4}; }), "RX gain too low");
        expect(one([](ConfigRequest &r) { r.tx_gain = std::vector<double>{90}; }), "TX gain too high");
        expect(one([](ConfigRequest &r) { r.tx_gain = std::vector<double>{1, 2, 3}; }), "three gains");
        expect(one([](ConfigRequest &r) { r.rx_channels = std::vector<double>{3}; }), "channel 3");
        expect(one([](ConfigRequest &r) { r.rx_channels = std::vector<double>{2, 1}; }), "descending channels");
        expect(one([](ConfigRequest &r) { r.rx_channels = std::vector<double>{1.5}; }), "fractional channel");
        expect(one([](ConfigRequest &r) { r.iq_bits = 16; }), "IQBits 16");
        expect(one([](ConfigRequest &r) { r.rx_gain_mode = std::string("auto"); }), "unknown gain mode");
    }

    {
        // Both directions off: rejected before the device sees it.
        ConfigRequest r;
        r.rx_channels = std::vector<double>{};
        expect(rejects(defaults(), r), "RxChannels [] with TxChannels []");
        r.tx_channels = std::vector<double>{1};
        Warnings w;
        const och_config c = apply(defaults(), r, w);
        expect(c.rx_mask == 0 && c.tx_mask == 1, "TX only");
    }
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::printf("config_request_test: all checks passed\n");
    return EXIT_SUCCESS;
}
