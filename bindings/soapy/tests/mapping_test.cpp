// Offline test of the SoapySDR <-> och_config value mappings (bindings/soapy/Mapping.hpp).
#include "Mapping.hpp"
#include <cstdio>

using namespace omnicore_soapy;

static int errors = 0;
#define EXPECT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            ++errors;                                                                                                  \
        }                                                                                                              \
    } while (0)

template<class F>
static bool throws(F f) {
    try {
        f();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

int main() {
    // TX gain follows Pluto: gain = 89.75 - attenuation, 0.25 dB steps, clamped.
    EXPECT(tx_gain_to_attenuation(0) == 89.75);
    EXPECT(tx_gain_to_attenuation(89.75) == 0);
    EXPECT(tx_gain_to_attenuation(79.75) == 10);
    EXPECT(tx_gain_to_attenuation(50.1) == 89.75 - 50.0);
    EXPECT(tx_gain_to_attenuation(50.2) == 89.75 - 50.25);
    EXPECT(tx_gain_to_attenuation(-5) == 89.75);
    EXPECT(tx_gain_to_attenuation(200) == 0);
    EXPECT(tx_attenuation_to_gain(10) == 79.75);
    for (double g = 0; g <= tx_attenuation_max; g += tx_gain_step)
        EXPECT(tx_attenuation_to_gain(tx_gain_to_attenuation(g)) == g);

    // RX gain: integer dB in -3..76.
    EXPECT(rx_gain_to_config(30.4, 2.45e9) == 30);
    EXPECT(rx_gain_to_config(30.6, 2.45e9) == 31);
    EXPECT(rx_gain_to_config(-10, 2.45e9) == -3);
    EXPECT(rx_gain_to_config(100, 1.0e9) == 73);
    EXPECT(rx_gain_to_config(100, 1.3e9) == 73);
    EXPECT(rx_gain_to_config(72, 1.31e9) == 71);
    EXPECT(rx_gain_to_config(72, 4.0e9) == 71);
    EXPECT(rx_gain_to_config(63, 4.01e9) == 62);
    EXPECT(rx_gain_to_config(62, 5.8e9) == 62);

    // Channels -> mask.
    EXPECT(channels_to_mask({}, 2) == 1);
    EXPECT(channels_to_mask({0}, 2) == 1);
    EXPECT(channels_to_mask({1}, 2) == 2);
    EXPECT(channels_to_mask({0, 1}, 2) == 3);
    EXPECT(throws([] { channels_to_mask({1, 0}, 2); }));
    EXPECT(throws([] { channels_to_mask({0, 0}, 2); }));
    EXPECT(throws([] { channels_to_mask({2}, 2); }));
    EXPECT(throws([] { channels_to_mask({1}, 1); }));

    // Names <-> och values.
    EXPECT(agc_mode_index("manual") == 0);
    EXPECT(agc_mode_index("slow") == 2);
    EXPECT(throws([] { agc_mode_index("auto"); }));
    EXPECT(source_index(clock_sources(), "external", "clock source") == 1);
    EXPECT(source_index(time_sources(), "sync", "time source") == 3);
    EXPECT(throws([] { source_index(time_sources(), "gps", "time source"); }));
    EXPECT(parse_bool("true") && parse_bool("1") && !parse_bool("off"));
    EXPECT(throws([] { parse_bool("yes"); }));

    if (errors) std::printf("%d failure(s)\n", errors);
    else std::printf("soapy mapping: all checks passed\n");
    return errors ? 1 : 0;
}
