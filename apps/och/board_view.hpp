#pragma once
#include <omnicore/och.h>
#include <iomanip>
#include <locale>
#include <sstream>

namespace och_cli::board_view {
    inline const char *state_name(uint32_t state) {
        const char *names[] = {"off", "initializing", "idle", "rx", "tx", "duplex", "recovering", "fault", "overtemp"};
        return state <= OCH_RGB_OVERTEMP ? names[state] : "unknown";
    }
    inline std::string rgb(const och_rgb_status &status) {
        std::ostringstream out;
        out << "rgb_status available=" << status.available << " enabled=" << status.enabled
            << " brightness_pct=" << status.brightness_pct << " state=" << state_name(status.state)
            << " red=" << status.red << " green=" << status.green << " blue=" << status.blue
            << " blink_hz=" << status.blink_hz << '\n';
        return out.str();
    }
    inline std::string fan(const och_fan_status &status) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "fan_status available=" << status.available << " enabled=" << status.enabled
            << " frequency_hz=" << status.frequency_hz << " duty_permille=" << status.duty_permille
            << " target_permille=" << status.target_permille << " boost=" << status.boost
            << " failsafe=" << status.failsafe << " temp_valid=" << status.temperature_valid << " temp_mC=";
        if (status.temperature_valid)
            out << status.temperature_mc;
        else
            out << "unknown";
        out << " temp_age_ms=" << status.temperature_age_ms << " overtemp=" << status.overtemperature
            << " source=zynq duty_pct=" << std::fixed << std::setprecision(1) << status.duty_permille / 10.0 << '\n';
        return out.str();
    }
    inline std::string rssi(const och_rx_rssi &status) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "rx_rssi enabled=" << status.enabled << " usb_rx_mask=" << status.usb_rx_mask;
        for (unsigned i = 0; i < 2; ++i) {
            out << " rx" << i + 1 << "_db=";
            if (status.valid[i])
                out << std::fixed << std::setprecision(2) << status.rssi_db[i];
            else
                out << "unknown";
        }
        out << " unit=dB_relative calibrated=0\n";
        return out.str();
    }
    inline std::string temperature(const och_core_temperature &status) {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "core_temp source=zynq valid=" << status.valid << " milli_celsius=";
        if (status.valid)
            out << status.milli_celsius;
        else
            out << "unknown";
        out << " age_ms=" << status.age_ms << " overtemp=" << status.overtemperature << " Celsius=";
        if (status.valid)
            out << std::fixed << std::setprecision(3) << status.milli_celsius / 1000.0;
        else
            out << "unknown";
        out << '\n';
        return out.str();
    }
}
