// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "options.hpp"
#include <algorithm>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

// The public command namespace uses Hz, dB and normalized amplitude. Legacy
// SDK commands intentionally keep their original names and units elsewhere.
namespace och_cli::commands {
    enum class Operation { query, set, action };
    struct Argument {
        std::string name, unit;
        double minimum = 0, maximum = 0, step = 1, sdk_scale = 1;
        unsigned precision = 0;
        bool hexadecimal = false;
        std::vector<std::pair<std::string, int>> values;
    };
    struct CatalogEntry {
        std::string name, query, setter, action, summary, unit, sdk_unit;
        std::vector<Argument> query_args, set_args;
        bool supported = true;
        bool shared_clock = false;
        std::string legacy_setter{}; // Raw compatibility only; never used by translate.
    };
    inline Argument numeric(std::string name, std::string unit, double low, double high, double step = 1,
                            double sdk_scale = 1, unsigned precision = 0) {
        return {std::move(name), std::move(unit), low, high, step, sdk_scale, precision, false, {}};
    }
    inline Argument boolean(std::string name) {
        auto a = numeric(std::move(name), "boolean", 0, 1);
        a.values = {{"off", 0}, {"on", 1}, {"false", 0}, {"true", 1}};
        return a;
    }
    inline const std::vector<CatalogEntry> &catalog() {
        static const auto all = [] {
            std::vector<CatalogEntry> v;
            auto query = [&](std::string name, std::string sdk, std::string summary, std::string unit = "") {
                v.push_back({std::move(name),
                             std::move(sdk) + "?",
                             "",
                             "",
                             std::move(summary),
                             unit,
                             unit,
                             {},
                             {},
                             true,
                             false});
            };
            auto scalar = [&](std::string name, std::string sdk, std::string summary, Argument arg,
                              std::string sdk_unit = "") {
                v.push_back({std::move(name),
                             sdk + "?",
                             sdk + "=",
                             "",
                             std::move(summary),
                             arg.unit,
                             sdk_unit.empty() ? arg.unit : std::move(sdk_unit),
                             {},
                             {std::move(arg)},
                             true,
                             false});
            };
            auto action = [&](std::string name, std::string sdk, std::string summary) {
                v.push_back({std::move(name),
                             "",
                             "",
                             std::move(sdk) + "!",
                             std::move(summary),
                             "",
                             "",
                             {},
                             {},
                             true,
                             false});
            };
            query("system.help", "help", "Firmware command help, including original SDK names and units");
            query("host.protocol", "host_protocol", "Control envelope and protocol capabilities");
            query("rf.register", "register", "Read one AD9361 register; address accepts decimal or 0x hexadecimal",
                  "byte");
            auto address = numeric("address", "register address", 0, 1023);
            address.hexadecimal = true;
            v.back().query_args = {address};
            for (const std::string direction: {"rx", "tx"}) {
                auto lo_source = numeric("source", "enum", 0, 1);
                lo_source.values = {{"internal", 0}, {"external", 1}};
                scalar(direction + ".lo-source", direction + "_lo_source",
                       "Internal PLL (default) or external LO input; external input is twice nominal RF frequency, up to 4 GHz RF",
                       lo_source);
                scalar(direction + ".lo-frequency", direction + "_lo_freq", "RF local oscillator frequency",
                       numeric("frequency", "Hz", 70000000, 6000000000.0, 1, 1e-6, 6), "MHz");
                scalar(direction + ".sample-rate", direction + "_samp_freq",
                       "Shared RX/TX sample clock; changing either direction changes both; FIR/clock limits apply",
                       numeric("sample-rate", "Hz", 1, 61440000));
                v.back().shared_clock = true;
                v.back().legacy_setter = v.back().setter;
                v.back().setter = "clock_sample_rate=";
                scalar(direction + ".rf-bandwidth", direction + "_rf_bandwidth", "Analog RF filter bandwidth",
                       numeric("bandwidth", "Hz", 200000, 56000000));
                scalar(direction + ".fir-enabled", direction + "_fir_en",
                       "Enable the currently loaded AD9361 FIR; prefer och fir for a complete FIR transaction",
                       boolean("enabled"));
                v.back().shared_clock = true;
                scalar(direction + ".fir-profile", direction + "_fir_config",
                       "Legacy built-in FIR profile 0; loading disables that FIR; prefer och fir load",
                       numeric("profile", "profile id", 0, 0));
                v.back().shared_clock = true;
                auto mask = numeric("channels", "channel mask", 0, 3);
                mask.values = {{"off", 0}, {"ch1", 1}, {"ch2", 2}, {"both", 3}, {"1,2", 3}};
                scalar(direction + ".channels", direction + "_channel", "Enabled channels: off=0, ch1=1, ch2=2, both=3",
                       mask);
            }
            scalar("rx.dc-correction", "rx_dc_correction",
                   "AD9361 RF and baseband DC tracking for both RX channels; requires SDK rx_correction=1",
                   boolean("enabled"));
            scalar("rx.iq-correction", "rx_iq_correction",
                   "AD9361 quadrature tracking for both RX channels; requires SDK rx_correction=1", boolean("enabled"));
            for (const std::string ch: {"1", "2"}) {
                auto mode = numeric("mode", "enum", 0, 3);
                mode.values = {{"manual", 0}, {"fast", 1}, {"slow", 2}, {"hybrid", 3}};
                scalar("rx" + ch + ".gain-mode", "rx" + ch + "_gc_mode", "Receive gain control mode", mode);
                scalar("rx" + ch + ".gain", "rx" + ch + "_rf_gain",
                       "Manual integer receive gain; current RF band may further restrict the range",
                       numeric("gain", "dB", -3, 76));
                scalar("tx" + ch + ".attenuation", "tx" + ch + "_attenuation",
                       "Requested TX attenuation; TX off keeps physical output at maximum attenuation",
                       numeric("attenuation", "dB", 0, 89.75, 0.25, 1000), "mdB");
                for (const std::string tone: {"1", "2"}) {
                    auto name = "dds.tx" + ch + ".tone" + tone;
                    auto sdk = "dds_tx" + ch + "_tone" + tone;
                    scalar(name + ".frequency", sdk + "_freq",
                           "DDS tone frequency; firmware also requires frequency <= current TX sample-rate / 2",
                           numeric("frequency", "Hz", 0, 4294967295.0));
                    scalar(name + ".phase", sdk + "_phase",
                           "DDS I phase; Q follows I + 270 degrees; 360 degrees wraps to 0",
                           numeric("phase", "degrees", 0, 360, 0.001, 1, 3));
                    scalar(name + ".scale", sdk + "_scale",
                           "Signed normalized DDS amplitude; sum of both tones can clip",
                           numeric("scale", "ratio", -1, 1, 0.000001, 1000000), "micro-units");
                }
            }
            auto source = numeric("source", "enum", 0, 1);
            source.values = {{"dds", 0}, {"usb", 1}};
            scalar("tx.source", "tx_source", "Select DDS or USB data; stop TX before changing", source);
            scalar("tx.enabled", "tx_enable", "Explicit TX gate; USB mode requires prefilled data", boolean("enabled"));
            auto iq_bits = numeric("bits", "bits", 8, 12, 4);
            iq_bits.values = {{"sc8", 8}, {"packed12", 12}};
            scalar("usb.iq-bits", "iq_format",
                   "USB IQ precision: default 12; 8 requires both RX/TX channel masks=3 and USB TX source; stop both directions before switching",
                   iq_bits);
            query("usb.data-enabled", "usb_data_en", "RX/TX USB path enables in RX,TX order", "boolean pair");
            v.back().setter = "usb_data_en=";
            v.back().set_args = {boolean("rx-enabled"), boolean("tx-enabled")};
            query("clock.chain", "clock_chain",
                  "Read reference and RX/TX clock chains in Hz; use och clock for complete clock-chain transactions",
                  "Hz");
            v.back().shared_clock = true;
            scalar("clock.sample-rate", "clock_sample_rate",
                   "Set and verify a driver-generated shared RX/TX clock chain; FIR limits apply",
                   numeric("sample-rate", "Hz", 1, 61440000));
            v.back().query = "rx_samp_freq?";
            v.back().shared_clock = true;
            scalar("rgb.enabled", "rgb_enable",
                   "RGB status indicator (default on, brightness 15%; legacy 25% supported)", boolean("enabled"));
            query("rgb.status", "rgb_status", "RGB state, brightness, color and blink rate");
            scalar("fan.enabled", "fan_enable",
                   "Automatic Zynq temperature fan control (default off); off requests 0% PWM", boolean("enabled"));
            query("fan.status", "fan_status", "Fan PWM duty, target, startup boost, failsafe and Zynq temperature");
            query("core.temperature", "core_temp", "Zynq die temperature; invalid measurements remain unknown");
            scalar("rx.rssi-enabled", "rssi_enable", "RX RSSI reporting; off after power-up until enabled",
                   boolean("enabled"));
            query("rx.rssi", "rx_rssi",
                  "RX1/RX2 symbol RSSI in relative dB (higher is stronger); not calibrated input power; needs rx.rssi-enabled on");
            query("system.info", "system_info", "Board, firmware and data-format capabilities");
            query("system.status", "system_status", "RF, temperature, RSSI, stream and fault status");
            auto reference_source = numeric("source", "enum", 0, 1);
            reference_source.values = {{"internal", 0}, {"external", 1}};
            scalar("clock.ref-source", "clock_ref_source",
                   "Free-running internal 40 MHz oscillator or ADF4002 discipline from external 10 MHz (after the CH32 releases PA4); lock is not readable",
                   reference_source);
            query("clock.ref-status", "adf4002_status",
                  "Reference configuration and GPIO checks; LOCK: CH32 PPS loop (PPS only) or PPS within +/-1 ppm (10M+PPS); PLL lock is D5 only");
            action("clock.ref-test", "adf4002_self_test",
                   "Flash D5 three times and restore source; GPIO readback only, human LED confirmation required");
            scalar("pps.input-enabled", "pps_input_en",
                   "PPS input (default off); with internal reference the CH32 PA4 DAC disciplines 40 MHz, with external 10 MHz it is a reference check only",
                   boolean("enabled"));
            query("pps.status", "pps_status",
                  "PPS presence, DATA_CLK frequency and +/-1 ppm frequency LOCK; hidden without a valid measurement");
            query("clock.vcxo-status", "vcxo_status",
                  "40 MHz tune mode (0 off, 1 10M, 2 PPS, 3 10M+PPS), ADF4002/PA4 ownership and CH32 PPS loop state");
            action("rf.reinit", "rf_reinit",
                   "Reinitialize RF and PL peripherals from the SDK saved configuration; interrupts streams");
            action("digital.self-test", "digital_self_test",
                   "Run the FPGA/AD9361 digital interface self-test while streams are stopped");
            auto bits = numeric("bits", "bit mask", 0, 127);
            bits.hexadecimal = true;
            scalar("usb.config", "usb_config",
                   "Advanced bridge CTRL0 bits; firmware state policy and TX gating still apply", bits);
            query("usb.rx", "usb_rx", "Advanced RX bridge flags: enabled, dual-channel, select-channel2");
            v.back().query.clear();
            v.back().setter = "usb_rx=";
            v.back().set_args = {boolean("enabled"), boolean("dual-channel"), boolean("select-channel2")};
            query("usb.tx", "usb_tx", "Advanced TX bridge flags: enabled, dual-channel, select-channel2, run");
            v.back().query.clear();
            v.back().setter = "usb_tx=";
            v.back().set_args = {boolean("enabled"), boolean("dual-channel"), boolean("select-channel2"),
                                 boolean("run")};
            query("usb.status", "usb_status", "USB bridge/FIFO status, faults and recovery counters");
            query("usb.irq", "usb_irq", "Interrupt state and retained fault history");
            action("usb.clear", "usb_clear", "Queue error clearing; preserves history and does not reset FIFOs");
            action("usb.stop", "usb_stop", "Stop USB paths; in USB TX source mode also stop TX output");
            std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
            return v;
        }();
        return all;
    }
    inline const CatalogEntry *find(const std::string &name) {
        const auto &v = catalog();
        auto it = std::lower_bound(v.begin(), v.end(), name, [](const auto &a, const auto &b) { return a.name < b; });
        return it != v.end() && it->name == name ? &*it : nullptr;
    }
    inline bool known(const std::string &name) { return find(name) != nullptr; }
    inline std::string format(double value, unsigned precision) {
        if (value == 0) value = 0; // Suppress negative zero.
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << std::fixed << std::setprecision(precision) << value;
        auto text = s.str();
        if (precision) {
            while (text.back() == '0') text.pop_back();
            if (text.back() == '.') text.pop_back();
        }
        return text;
    }
    inline double decimal(const std::string &text) {
        if (text.empty()) throw std::invalid_argument("empty numeric argument");
        std::string input = text;
        double multiplier = 1;
        if (input.back() == 'k' || input.back() == 'K') {
            multiplier = 1e3;
            input.pop_back();
        } else if (input.back() == 'M') {
            multiplier = 1e6;
            input.pop_back();
        } else if (input.back() == 'G') {
            multiplier = 1e9;
            input.pop_back();
        }
        size_t p = 0;
        if (p < input.size() && (input[p] == '+' || input[p] == '-')) ++p;
        bool digit = false;
        while (p < input.size() && input[p] >= '0' && input[p] <= '9') {
            digit = true;
            ++p;
        }
        if (p < input.size() && input[p] == '.') {
            ++p;
            while (p < input.size() && input[p] >= '0' && input[p] <= '9') {
                digit = true;
                ++p;
            }
        }
        if (!digit) throw std::invalid_argument("expected decimal number: " + text);
        if (p < input.size() && (input[p] == 'e' || input[p] == 'E')) {
            ++p;
            if (p < input.size() && (input[p] == '+' || input[p] == '-')) ++p;
            const auto start = p;
            while (p < input.size() && input[p] >= '0' && input[p] <= '9') ++p;
            if (p == start) throw std::invalid_argument("invalid exponent: " + text);
        }
        if (p != input.size()) throw std::invalid_argument("invalid numeric argument: " + text);
        std::istringstream s(input);
        s.imbue(std::locale::classic());
        double value = 0;
        s >> value;
        if (!s || !s.eof() || !std::isfinite(value * multiplier))
            throw std::invalid_argument("numeric argument out of range: " + text);
        return value * multiplier;
    }
    inline double argument_value(const Argument &a, const std::string &text) {
        for (const auto &v: a.values)
            if (text == v.first) return v.second;
        double value;
        if (a.hexadecimal && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            uint64_t n = 0;
            for (size_t i = 2; i < text.size(); ++i) {
                unsigned c = static_cast<unsigned char>(text[i]);
                unsigned d = c >= '0' && c <= '9'   ? c - '0'
                             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                    : 16;
                if (d == 16 || n > (UINT64_MAX - d) / 16)
                    throw std::invalid_argument("invalid hexadecimal argument: " + text);
                n = n * 16 + d;
            }
            value = static_cast<double>(n);
        } else
            value = decimal(text);
        if (value < a.minimum || value > a.maximum)
            throw std::invalid_argument(a.name + " out of range [" + format(a.minimum, 6) + ", " +
                                        format(a.maximum, 6) + "] " + a.unit);
        if (a.step > 0) {
            const double scaled = value / a.step;
            // Tolerate only binary representation error of allowed decimal steps;
            // never round a fractional Hz into a different command.
            const double rounded = std::round(scaled) * a.step;
            const double tolerance =
                    std::numeric_limits<double>::epsilon() * std::max(std::abs(value), std::abs(rounded)) * 4;
            if ((a.step == 1 && value != std::floor(value)) || std::abs(value - rounded) > tolerance)
                throw std::invalid_argument(a.name + " must use increments of " + format(a.step, 6) + " " + a.unit);
        }
        return value;
    }
    inline std::string translate(const std::string &name, const std::vector<std::string> &args, Operation op) {
        auto e = find(name);
        if (!e) throw std::invalid_argument("unknown command: " + name);
        if (!e->supported) throw std::invalid_argument(name + " is reserved and unsupported by this SDK/hardware");
        const auto &prefix = op == Operation::query ? e->query : op == Operation::set ? e->setter : e->action;
        if (prefix.empty()) throw std::invalid_argument("operation not supported for " + name);
        const std::vector<Argument> none;
        const auto &parameters = op == Operation::query ? e->query_args : op == Operation::set ? e->set_args : none;
        if (args.size() != parameters.size())
            throw std::invalid_argument(name + " requires " + std::to_string(parameters.size()) + " arguments");
        std::string out = prefix;
        for (size_t i = 0; i < args.size(); ++i) {
            const auto &a = parameters[i];
            const auto value = argument_value(a, args[i]);
            if (i) out += ' ';
            out += format(value * a.sdk_scale, a.precision);
        }
        // @4294967295, space and LF consume 13 bytes of the SDK 128-byte line.
        if (out.size() > 115) throw std::invalid_argument("translated command exceeds SDK control-line capacity");
        return out;
    }
    // Normalize the primary scalar response, while retaining all other diagnostic
    // fields verbatim. Never rewrite status records whose contents are structured.
    inline std::string canonical_reply(const std::string &name, const std::string &reply) {
        const auto *e = find(name);
        if (!e || e->set_args.size() != 1 || !e->query_args.empty()) return reply;
        const auto &a = e->set_args.front();
        if (e->query.empty() || e->query.back() != '?') return reply;
        auto key = e->query.substr(0, e->query.size() - 1) + "=";
        if (reply.rfind(key, 0) != 0) {
            // Shared sample-rate writes use clock_sample_rate= for all three
            // canonical names, while each query keeps its legacy read command.
            if (e->setter.empty() || e->setter.back() != '=' || reply.rfind(e->setter, 0) != 0) return reply;
            key = e->setter;
        }
        const auto end = reply.find_first_of(" \r\n", key.size());
        const auto scalar = reply.substr(key.size(), end == std::string::npos ? end : end - key.size());
        try {
            double value = decimal(scalar) / a.sdk_scale;
            unsigned precision = e->unit == "Hz"        ? 0
                                 : e->unit == "dB"      ? 3
                                 : e->unit == "degrees" ? 3
                                 : e->unit == "ratio"   ? 6
                                                        : 0;
            return e->name + "=" + format(value, precision) + (end == std::string::npos ? "" : reply.substr(end));
        } catch (const std::invalid_argument &) {
            return reply;
        }
    }
    inline std::string argument_json(const Argument &a) {
        std::string out = "{\"name\":" + json(a.name) + ",\"unit\":" + json(a.unit) +
                          ",\"minimum\":" + format(a.minimum, 6) + ",\"maximum\":" + format(a.maximum, 6) +
                          ",\"step\":" + format(a.step, 6) + ",\"sdk_scale\":" + format(a.sdk_scale, 6) +
                          ",\"hexadecimal\":" + (a.hexadecimal ? "true" : "false") + ",\"values\":{";
        bool comma = false;
        for (const auto &v: a.values) {
            if (comma) out += ',';
            comma = true;
            out += json(v.first) + ":" + std::to_string(v.second);
        }
        return out + "}}";
    }
    inline std::string catalog_json() {
        std::string out = "[";
        bool comma = false;
        for (const auto &e: catalog()) {
            if (comma) out += ',';
            comma = true;
            out += "{\"name\":" + json(e.name) + ",\"summary\":" + json(e.summary) + ",\"unit\":" + json(e.unit) +
                   ",\"sdk_unit\":" + json(e.sdk_unit) + ",\"supported\":" + (e.supported ? "true" : "false") +
                   ",\"shared_clock\":" + (e.shared_clock ? "true" : "false") + ",\"query\":" + json(e.query) +
                   ",\"set\":" + json(e.setter) + ",\"action\":" + json(e.action) +
                   ",\"legacy_set_raw\":" + json(e.legacy_setter);
            auto args = [&](const char *name, const auto &parameters) {
                out += ",\"" + std::string(name) + "\":[";
                bool delim = false;
                for (const auto &a: parameters) {
                    if (delim) out += ',';
                    delim = true;
                    out += argument_json(a);
                }
                out += ']';
            };
            args("query_args", e.query_args);
            args("set_args", e.set_args);
            out += '}';
        }
        return out + "]";
    }
    inline std::string catalog_text() {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        std::string previous_group;
        for (const auto &e: catalog()) {
            const auto group = e.name.substr(0, e.name.find('.'));
            if (group != previous_group) {
                out << "\n  [ " << group << " ]\n  " << std::string(68, '-') << '\n';
                previous_group = group;
            }
            out << "  " << e.name << " [";
            bool first = true;
            if (!e.query.empty()) {
                out << "get";
                first = false;
            }
            if (!e.setter.empty()) {
                out << (first ? "" : "/") << "set";
                first = false;
            }
            if (!e.action.empty()) out << (first ? "" : "/") << "action";
            out << "]" << (e.supported ? "" : " RESERVED/UNSUPPORTED");
            if (!e.unit.empty()) out << " (" << e.unit << ")";
            out << '\n';
            std::istringstream words(e.summary);
            std::string word;
            size_t column = 4;
            out << "    ";
            while (words >> word) {
                if (column > 4 && column + word.size() + 1 > 74) {
                    out << "\n    ";
                    column = 4;
                }
                if (column > 4) {
                    out << ' ';
                    ++column;
                }
                out << word;
                column += word.size();
            }
            out << '\n';
            auto args = [&](const char *operation, const auto &parameters) {
                for (const auto &a: parameters) {
                    out << "    " << operation << " " << a.name << ": " << format(a.minimum, 6) << ".."
                        << format(a.maximum, 6) << " " << a.unit << ", step " << format(a.step, 6);
                    for (const auto &v: a.values) out << " " << v.first << "=" << v.second;
                    out << '\n';
                }
            };
            args("get", e.query_args);
            args("set", e.set_args);
        }
        return out.str();
    }
} // namespace och_cli::commands
