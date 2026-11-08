// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <omnicore/och.h>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

// One display policy for canonical commands, SDK aliases and system status.
// Raw SDK replies remain available separately for protocol diagnostics.
namespace och_cli::monitor_view {
    inline bool applies(const std::string &command) {
        const auto name = command.substr(0, command.find_first_of("?=!"));
        return name == "clock_ref_source" || name == "adf4002_status" || name == "pps_input_en" ||
               name == "pps_status" || name == "system_status";
    }
    // PPS without 10 MHz: the CH32 PA4 phase loop decides LOCK. Otherwise the
    // PPS +/-1 ppm frequency check (10 MHz + PPS) or nothing (10 MHz only).
    inline std::string lock_fields(const och_pps_status *pps, const och_vcxo_status *vcxo = nullptr) {
        if (vcxo && vcxo->requested_mode == OCH_VCXO_PPS) {
            if (!vcxo->bridge_alive) return {};
            return " locked=" + std::to_string(vcxo->locked) + " lock_source=CH32_PPS_LOOP";
        }
        if (!pps || !pps->lock_known) return {};
        return " locked=" + std::to_string(pps->locked) +
               " lock_source=PPS lock_tolerance_ppm=" + std::to_string(OCH_PPS_LOCK_TOLERANCE_PPM);
    }
    inline std::string pps_line(const och_pps_status &pps, const och_vcxo_status *vcxo = nullptr) {
        const char *states[] = {"disabled", "waiting", "valid", "no_signal", "invalid_interval", "clock_unavailable"};
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "pps_status available=" << pps.available << " enabled=" << pps.enabled
            << " state=" << (pps.state < 6 ? states[pps.state] : "unknown") << " signal_present=" << pps.signal_present
            << " frequency_valid=" << pps.frequency_valid << " nominal_hz=" << pps.nominal_hz
            << " measured_hz=" << pps.measured_hz << " error_ppm=";
        if (pps.frequency_valid)
            out << std::fixed << std::setprecision(6) << pps.error_ppm;
        else
            out << "unknown";
        out << " pulse_count=" << pps.pulse_count << " invalid_count=" << pps.invalid_count << " age_ms=" << pps.age_ms
            << " raw_counter=" << pps.raw_counter << " hardware_timeout=" << pps.hardware_timeout
            << " monitor_only=1 adjustment=0 clock=DATA_CLK expected_pps_hz=1" << lock_fields(&pps, vcxo);
        return out.str();
    }
    inline std::string reply(const std::string &raw, const och_pps_status *pps, const och_vcxo_status *vcxo = nullptr) {
        std::istringstream lines(raw);
        std::ostringstream out;
        std::string line;
        bool has_pps_line = false;
        while (std::getline(lines, line)) {
            std::istringstream words(line);
            std::string token;
            bool first = true;
            const auto start = line.find_first_not_of(" \t\r");
            if (start == std::string::npos) continue;
            if (line.compare(start, 11, "pps_status ") == 0) {
                // Use the same snapshot for frequency, presence and lock.
                out << (pps ? pps_line(*pps, vcxo) : "pps_status unavailable") << '\n';
                has_pps_line = true;
                continue;
            }
            const bool reference = line.find("adf4002_status") != std::string::npos ||
                                   line.find("clock_ref_source=") != std::string::npos;
            while (words >> token) {
                const auto key = token.substr(0, token.find('='));
                if (reference && (key == "lock_known" || key == "locked" || key == "led_known" || key == "led_on"))
                    continue;
                if (!first) out << ' ';
                out << token;
                first = false;
            }
            out << '\n';
        }
        if (!has_pps_line && !lock_fields(pps, vcxo).empty()) out << "pps_monitor" << lock_fields(pps, vcxo) << '\n';
        return out.str();
    }
} // namespace och_cli::monitor_view
